namespace Recorder.Capture;

internal static unsafe class VideoFrameContentAnalyzer
{
    public static bool IsBgraFrameEmpty(byte[] buffer, int width, int height, int stride)
    {
        int x0 = width / 4;
        int x1 = width / 2;
        int x2 = width * 3 / 4;
        int y0 = height / 4;
        int y1 = height / 2;
        int y2 = height * 3 / 4;

        return IsBgraSampleEmpty(buffer, stride, x0, y0) &&
               IsBgraSampleEmpty(buffer, stride, x1, y0) &&
               IsBgraSampleEmpty(buffer, stride, x2, y0) &&
               IsBgraSampleEmpty(buffer, stride, x0, y1) &&
               IsBgraSampleEmpty(buffer, stride, x1, y1) &&
               IsBgraSampleEmpty(buffer, stride, x2, y1) &&
               IsBgraSampleEmpty(buffer, stride, x0, y2) &&
               IsBgraSampleEmpty(buffer, stride, x1, y2) &&
               IsBgraSampleEmpty(buffer, stride, x2, y2);
    }

    public static bool IsNv12FrameEmpty(byte* data, int width, int height)
    {
        int x0 = width / 4;
        int x1 = width / 2;
        int x2 = width * 3 / 4;
        int y0 = height / 4;
        int y1 = height / 2;
        int y2 = height * 3 / 4;

        return IsNv12SampleEmpty(data, width, height, x0, y0) &&
               IsNv12SampleEmpty(data, width, height, x1, y0) &&
               IsNv12SampleEmpty(data, width, height, x2, y0) &&
               IsNv12SampleEmpty(data, width, height, x0, y1) &&
               IsNv12SampleEmpty(data, width, height, x1, y1) &&
               IsNv12SampleEmpty(data, width, height, x2, y1) &&
               IsNv12SampleEmpty(data, width, height, x0, y2) &&
               IsNv12SampleEmpty(data, width, height, x1, y2) &&
               IsNv12SampleEmpty(data, width, height, x2, y2);
    }

    private static bool IsBgraSampleEmpty(byte[] buffer, int stride, int x, int y)
    {
        int idx = y * stride + x * 4;
        return buffer[idx] == 0 &&
               buffer[idx + 1] == 0 &&
               buffer[idx + 2] == 0 &&
               buffer[idx + 3] == 0;
    }

    private static bool IsNv12SampleEmpty(byte* data, int width, int height, int x, int y)
    {
        int evenX = x & ~1;
        int evenY = y & ~1;
        int uvBase = width * height + (evenY / 2) * width + evenX;
        return data[y * width + x] == 0 &&
               data[uvBase] == 0 &&
               data[uvBase + 1] == 0;
    }
}
