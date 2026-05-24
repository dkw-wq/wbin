#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include <opencv2/core.hpp>
#include <onnxruntime_cxx_api.h>

struct FaceInfo {
    float x1, y1, x2, y2;
    float score;
};

class FaceDetector {
public:
    FaceDetector(const std::string& modelPath);
    ~FaceDetector() = default;

    std::vector<FaceInfo> detect(const cv::Mat& bgrFrame,
                                  float detThresh = 0.5f,
                                  float nmsThresh = 0.4f);

private:
    Ort::Env env{nullptr};
    Ort::Session session{nullptr};
    Ort::MemoryInfo memInfo{nullptr};
    Ort::AllocatorWithDefaultOptions allocator;

    std::vector<const char*> inputNames;
    std::vector<const char*> outputNames;
    std::vector<Ort::AllocatedStringPtr> inputNamesPtr;
    std::vector<Ort::AllocatedStringPtr> outputNamesPtr;

    int64_t inputH = -1, inputW = -1;

    int fmc = 3;
    int numAnchors = 2;
    int strides[3] = {8, 16, 32};
    float inputMean = 127.5f;
    float inputStd = 128.0f;

    struct AnchorKey {
        int h, w, stride;
        bool operator==(const AnchorKey& o) const {
            return h == o.h && w == o.w && stride == o.stride;
        }
    };
    struct AnchorKeyHash {
        size_t operator()(const AnchorKey& k) const {
            return size_t((k.h * 31 + k.w) * 31 + k.stride);
        }
    };
    std::unordered_map<AnchorKey, std::vector<float>, AnchorKeyHash> centerCache;

    std::vector<float> getAnchors(int h, int w, int stride);
    void nms(std::vector<FaceInfo>& faces, float nmsThresh);
};
