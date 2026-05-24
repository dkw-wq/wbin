#include "face_detect.h"
#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>

FaceDetector::FaceDetector(const std::string& modelPath)
    : env(ORT_LOGGING_LEVEL_WARNING, "face_detect"),
      session(nullptr),
      memInfo(nullptr) {
    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(2);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_EXTENDED);

    session = Ort::Session(env, modelPath.c_str(), opts);

    memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    // Get input info
    size_t numInputs = session.GetInputCount();
    for (size_t i = 0; i < numInputs; i++) {
        auto name = session.GetInputNameAllocated(i, allocator);
        inputNamesPtr.push_back(std::move(name));
        inputNames.push_back(inputNamesPtr.back().get());

        auto typeInfo = session.GetInputTypeInfo(i);
        auto shapeInfo = typeInfo.GetTensorTypeAndShapeInfo();
        auto shape = shapeInfo.GetShape();
        if (shape.size() == 4) {
            inputH = shape[2] > 0 ? shape[2] : -1;
            inputW = shape[3] > 0 ? shape[3] : -1;
        }
    }

    // Get output info
    size_t numOutputs = session.GetOutputCount();
    for (size_t i = 0; i < numOutputs; i++) {
        auto name = session.GetOutputNameAllocated(i, allocator);
        outputNamesPtr.push_back(std::move(name));
        outputNames.push_back(outputNamesPtr.back().get());
    }
}

std::vector<float> FaceDetector::getAnchors(int h, int w, int stride) {
    AnchorKey key{h, w, stride};
    auto it = centerCache.find(key);
    if (it != centerCache.end()) {
        return it->second;
    }

    std::vector<float> anchors;
    anchors.reserve(h * w * numAnchors * 2);

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float cx = (x + 0.5f) * stride;
            float cy = (y + 0.5f) * stride;
            for (int a = 0; a < numAnchors; a++) {
                anchors.push_back(cx);
                anchors.push_back(cy);
            }
        }
    }

    if (centerCache.size() < 100) {
        centerCache[key] = anchors;
    }
    return anchors;
}

void FaceDetector::nms(std::vector<FaceInfo>& faces, float nmsThresh) {
    if (faces.empty()) return;

    std::sort(faces.begin(), faces.end(),
              [](const FaceInfo& a, const FaceInfo& b) { return a.score > b.score; });

    std::vector<bool> removed(faces.size(), false);
    std::vector<FaceInfo> result;

    for (size_t i = 0; i < faces.size(); i++) {
        if (removed[i]) continue;
        result.push_back(faces[i]);

        float ix1 = faces[i].x1, iy1 = faces[i].y1;
        float ix2 = faces[i].x2, iy2 = faces[i].y2;
        float iarea = (ix2 - ix1 + 1) * (iy2 - iy1 + 1);

        for (size_t j = i + 1; j < faces.size(); j++) {
            if (removed[j]) continue;

            float xx1 = std::max(ix1, faces[j].x1);
            float yy1 = std::max(iy1, faces[j].y1);
            float xx2 = std::min(ix2, faces[j].x2);
            float yy2 = std::min(iy2, faces[j].y2);

            float w = std::max(0.0f, xx2 - xx1 + 1);
            float h = std::max(0.0f, yy2 - yy1 + 1);
            float inter = w * h;

            float jarea = (faces[j].x2 - faces[j].x1 + 1) * (faces[j].y2 - faces[j].y1 + 1);
            float ovr = inter / (iarea + jarea - inter);

            if (ovr > nmsThresh) {
                removed[j] = true;
            }
        }
    }

    faces = std::move(result);
}

std::vector<FaceInfo> FaceDetector::detect(const cv::Mat& bgrFrame,
                                            float detThresh,
                                            float nmsThresh) {
    int origH = bgrFrame.rows;
    int origW = bgrFrame.cols;

    // Letterbox padding to square (640x640)
    int targetSize = (inputH > 0) ? (int)inputH : 640;
    int newW, newH;
    float imRatio = (float)origH / origW;
    float modelRatio = (float)targetSize / targetSize; // 1.0 for square

    if (imRatio > modelRatio) {
        newH = targetSize;
        newW = (int)(newH / imRatio);
    } else {
        newW = targetSize;
        newH = (int)(newW * imRatio);
    }

    float detScale = (float)newH / origH;

    cv::Mat resized;
    cv::resize(bgrFrame, resized, cv::Size(newW, newH));

    cv::Mat padded(targetSize, targetSize, CV_8UC3, cv::Scalar(0, 0, 0));
    resized.copyTo(padded(cv::Rect(0, 0, newW, newH)));

    // Create blob: scale=1/128, mean=127.5, swapRB=true (BGR->RGB)
    cv::Mat blob = cv::dnn::blobFromImage(padded, 1.0f / inputStd,
                                           cv::Size(targetSize, targetSize),
                                           cv::Scalar(inputMean, inputMean, inputMean),
                                           true);

    // Run inference
    std::vector<int64_t> inputShape(blob.dims);
    for (int i = 0; i < blob.dims; i++) inputShape[i] = blob.size[i];

    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memInfo, (float*)blob.data,
        blob.total(), inputShape.data(), inputShape.size());

    std::vector<Ort::Value> outputTensors = session.Run(Ort::RunOptions{nullptr},
                                                         inputNames.data(),
                                                         &inputTensor, 1,
                                                         outputNames.data(),
                                                         outputNames.size());

    // Decode outputs
    int fmc = this->fmc;
    std::vector<FaceInfo> allFaces;

    for (int idx = 0; idx < fmc; idx++) {
        int stride = strides[idx];

        // Get output tensors for this scale
        float* scoresData = outputTensors[idx].GetTensorMutableData<float>();
        float* bboxData = outputTensors[idx + fmc].GetTensorMutableData<float>();
        // float* kpsData = outputTensors[idx + fmc * 2].GetTensorMutableData<float>();

        auto scoresShape = outputTensors[idx].GetTensorTypeAndShapeInfo().GetShape();
        int numPoints = (int)scoresShape[0];

        int inputHVal = targetSize;
        int inputWVal = targetSize;
        int height = inputHVal / stride;
        int width = inputWVal / stride;

        // Multiply bbox predictions by stride
        std::vector<float> bboxScaled(numPoints * 4);
        for (int i = 0; i < numPoints * 4; i++) {
            bboxScaled[i] = bboxData[i] * stride;
        }

        // Get anchor centers
        std::vector<float> anchors = getAnchors(height, width, stride);

        // Decode bboxes (distance2bbox)
        std::vector<float> boxes(numPoints * 4);
        for (int i = 0; i < numPoints; i++) {
            float ax = anchors[i * 2];
            float ay = anchors[i * 2 + 1];
            boxes[i * 4 + 0] = ax - bboxScaled[i * 4 + 0];
            boxes[i * 4 + 1] = ay - bboxScaled[i * 4 + 1];
            boxes[i * 4 + 2] = ax + bboxScaled[i * 4 + 2];
            boxes[i * 4 + 3] = ay + bboxScaled[i * 4 + 3];
        }

        // Filter by threshold
        for (int i = 0; i < numPoints; i++) {
            float score = scoresData[i];
            if (score >= detThresh) {
                FaceInfo face;
                // Scale back to original image coordinates
                face.x1 = boxes[i * 4 + 0] / detScale;
                face.y1 = boxes[i * 4 + 1] / detScale;
                face.x2 = boxes[i * 4 + 2] / detScale;
                face.y2 = boxes[i * 4 + 3] / detScale;
                face.score = score;
                allFaces.push_back(face);
            }
        }
    }

    // NMS
    nms(allFaces, nmsThresh);

    return allFaces;
}
