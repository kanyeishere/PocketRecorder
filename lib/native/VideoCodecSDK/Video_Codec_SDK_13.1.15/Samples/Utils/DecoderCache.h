/*
 * This copyright notice applies to this header file only:
 *
 * Copyright (c) 2010-2026 NVIDIA Corporation
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the software, and to permit persons to whom the
 * software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

//---------------------------------------------------------------------------
//! \file DecoderCache.h
//! \brief LRU cache for decoder instances
//!
//! Provides a cache to store and reuse decoder instances based on codec
//! properties (codec, bit depth, chroma format). This avoids expensive
//! decoder recreation when switching between videos with similar properties.
//---------------------------------------------------------------------------

#pragma once

#include <iostream>
#include <list>
#include <type_traits>
#include <unordered_map>
#include <tuple>
#include "cuviddec.h"

// Hash utility for tuples (C++11 compatible)
struct TupleHash {
    template <typename T>
    std::size_t operator()(const T& tuple) const {
        std::size_t seed = 0;
        hash_tuple_impl(tuple, seed);
        return seed;
    }

private:
    // Hash combining function
    template <typename T>
    static void hash_combine(std::size_t& seed, const T& value) {
        seed ^= std::hash<T>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
    }

    // Recursive template to hash tuple elements
    template <typename Tuple, std::size_t Index = 0>
    static typename std::enable_if<Index == std::tuple_size<Tuple>::value>::type
    hash_tuple_impl(const Tuple&, std::size_t&) {
        // Base case: no more elements
    }

    template <typename Tuple, std::size_t Index = 0>
    static typename std::enable_if<Index < std::tuple_size<Tuple>::value>::type
    hash_tuple_impl(const Tuple& tuple, std::size_t& seed) {
        hash_combine(seed, std::get<Index>(tuple));
        hash_tuple_impl<Tuple, Index + 1>(tuple, seed);
    }
};

// std::hash specialization for cudaVideoCodec
namespace std {
    template <>
    struct hash<cudaVideoCodec> {
        size_t operator()(const cudaVideoCodec& e) const {
            return std::hash<std::underlying_type<cudaVideoCodec>::type>()(
                static_cast<std::underlying_type<cudaVideoCodec>::type>(e));
        }
    };
}

// std::hash specialization for cudaVideoChromaFormat
namespace std {
    template <>
    struct hash<cudaVideoChromaFormat> {
        size_t operator()(const cudaVideoChromaFormat& e) const {
            return std::hash<std::underlying_type<cudaVideoChromaFormat>::type>()(
                static_cast<std::underlying_type<cudaVideoChromaFormat>::type>(e));
        }
    };
}

/**
 * @brief LRU cache for decoder instances
 * 
 * Caches decoder instances based on codec properties (codec, bit depth, chroma format).
 * When capacity is reached, least recently used (LRU) decoder is evicted.
 * 
 * OWNERSHIP MODEL:
 * - This cache stores RAW POINTERS (Value = NvDecoder*) without taking ownership
 * - The caller (e.g., NvVideoDecoder) maintains formal ownership via unique_ptr
 * - CRITICAL INVARIANT: Before resetting/deleting an object elsewhere, the caller MUST
 *   call release() on their unique_ptr to transfer ownership to the cache
 * - On eviction (PushDecoder returns non-nullptr), caller is responsible for deletion
 * - Cache capacity=0 disables caching (PushDecoder returns nullptr immediately)
 * 
 * TYPICAL USAGE PATTERN:
 * @code
 *   // Initial creation and caching
 *   m_pDecoder.reset(new NvDecoder(...));
 *   m_cache.PushDecoder(key, m_pDecoder.get());  // Cache stores raw pointer
 *   
 *   // Later, when switching decoders:
 *   NvDecoder* newDecoder = new NvDecoder(...);
 *   m_pDecoder.release();                        // Transfer ownership to cache
 *   m_pDecoder.reset(newDecoder);                // Take ownership of new decoder
 *   NvDecoder* evicted = m_cache.PushDecoder(key, newDecoder);
 *   if (evicted) delete evicted;                 // Clean up evicted decoder
 * @endcode
 * 
 * Template parameters:
 * @tparam Key - Cache key type (typically a tuple of codec properties)
 * @tparam Value - Cached value type (typically NvDecoder*)
 */
template<typename Key, typename Value>
class DecoderCache {
public:
    /**
     * @brief Construct a new Decoder Cache object
     * 
     * @param capacity Maximum number of decoders to cache (0 = disabled)
     */
    DecoderCache(uint32_t capacity = 0) : m_uCapacity(capacity) {
        // No validation needed: 0 = disabled, any positive value = cache size
    }

    /**
     * @brief Get decoder from cache
     * 
     * @param key Cache key (codec properties)
     * @return Value Decoder pointer if found, nullptr otherwise
     */
    Value GetDecoder(const Key& key) {
        if (m_uCapacity == 0) {
            return nullptr;  // Cache disabled
        }
        auto it = m_cacheMap.find(key);
        if (it == m_cacheMap.end()) {
            return nullptr;
        }
        // Move the accessed item to the front of the list (most recently used)
        m_cacheItems.splice(m_cacheItems.begin(), m_cacheItems, it->second);
        return it->second->second;
    }

    /**
     * @brief Add decoder to cache
     * 
     * If cache is full, evicts least recently used decoder.
     * If key already exists, updates the value.
     * 
     * @param key Cache key (codec properties)
     * @param value Decoder pointer to cache
     * @return Value Evicted decoder pointer if cache was full, nullptr otherwise
     */
    Value PushDecoder(const Key& key, const Value& value) {
        if (m_uCapacity == 0) {
            return nullptr;  // Cache disabled, don't store anything
        }
        
        Value evictedDecoder = nullptr;
        auto it = m_cacheMap.find(key);
        if (it != m_cacheMap.end()) {
            // Update the value and move to front
            it->second->second = value;
            m_cacheItems.splice(m_cacheItems.begin(), m_cacheItems, it->second);
        } else {
            if (m_cacheItems.size() >= m_uCapacity) {
                // Remove the least recently used item
                auto last = m_cacheItems.back();
                m_cacheMap.erase(last.first);
                evictedDecoder = last.second;
                m_cacheItems.pop_back();
            }
            // Insert the new item at the front
            m_cacheItems.emplace_front(key, value);
            m_cacheMap[key] = m_cacheItems.begin();
        }
        return evictedDecoder;
    }

    /**
     * @brief Remove and return the least recently used decoder
     * 
     * @return Value Removed decoder pointer, or nullptr if cache is empty
     */
    Value RemoveElement() {
        if (m_cacheItems.empty()) {
            return nullptr;
        }
        auto last = m_cacheItems.back();
        m_cacheMap.erase(last.first);
        auto decoder = last.second;
        m_cacheItems.pop_back();

        return decoder;
    }

    /**
     * @brief Get current cache size
     * 
     * @return size_t Number of items in cache
     */
    size_t Size() const {
        return m_cacheItems.size();
    }

    /**
     * @brief Check if cache is empty
     * 
     * @return true if cache is empty
     */
    bool IsEmpty() const {
        return m_cacheItems.empty();
    }

private:
    uint32_t m_uCapacity;
    using KeyValuePair = std::pair<Key, Value>;
    std::list<KeyValuePair> m_cacheItems;
    std::unordered_map<Key, typename std::list<KeyValuePair>::iterator, TupleHash> m_cacheMap;
};

