using System;

namespace Recorder.Recording;

internal sealed class SoftFpsGovernor
{
    private readonly Action<string> _logInfo;
    private bool _active;
    private int _currentFps;
    private int _credit;

    public SoftFpsGovernor(Action<string> logInfo)
    {
        _logInfo = logInfo;
    }

    public bool ShouldCapture(int targetFps)
    {
        targetFps = Math.Max(1, targetFps);
        int softFps = Math.Min(targetFps, Math.Max(30, targetFps / 2));
        if (softFps >= targetFps)
            return true;

        if (!_active || _currentFps != softFps)
        {
            _active = true;
            _currentFps = softFps;
            _credit = targetFps - softFps;
            _logInfo($"[Record] Encoder write pressure detected; temporarily reducing capture rate to {softFps}fps.");
        }

        _credit += softFps;
        if (_credit < targetFps)
            return false;

        _credit -= targetFps;
        return true;
    }

    public void Reset(bool log = true)
    {
        if (log && _active)
            _logInfo("[Record] Encoder write pressure cleared; restored target capture rate.");

        _active = false;
        _currentFps = 0;
        _credit = 0;
    }
}
