// Copyright (C) 2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <utils/images_capture.h>
#include <utils/slog.hpp>

#ifdef _WIN32
#include "w_dirent.hpp"
#else
#include <dirent.h>
#endif

#include <opencv2/imgcodecs.hpp>

#include <stdexcept>
#include <string>
#include <memory>
#include <fstream>

class InvalidInput : public std::runtime_error {
public:
    explicit InvalidInput(const std::string& message) noexcept
        : std::runtime_error(message) {}
};

class OpenError : public std::runtime_error {
public:
    explicit OpenError(const std::string& message) noexcept
        : std::runtime_error(message) {}
};

class ImreadWrapper : public ImagesCapture {
    cv::Mat img;
    bool canRead;

public:
    ImreadWrapper(const std::string &input, bool loop) : ImagesCapture{loop}, canRead{true} {
        auto startTime = std::chrono::steady_clock::now();

        std::ifstream file(input.c_str());
        if (!file.good())
            throw InvalidInput("Can't find the image by " + input);

        slog::info << "ImreadWrapper: image {" << input << "}, " << img.cols << "x" << img.rows << slog::endl;
        img = cv::imread(input);
        if(!img.data)
            throw OpenError("Can't open the image from " + input);
        else
            readerMetrics.update(startTime);
    }

    double fps() const override {return 1.0;}

    std::string getType() const override {return "IMAGE";}

    cv::Mat read() override {
        if (loop) return img.clone();
        if (canRead) {
            canRead = false;
            return img.clone();
        }
        return cv::Mat{};
    }
};

class DirReader : public ImagesCapture {
    std::vector<std::string> names;
    size_t fileId;
    size_t nextImgId;
    const size_t initialImageId;
    const size_t readLengthLimit;
    const std::string input;

public:
    DirReader(const std::string &input, bool loop, size_t initialImageId, size_t readLengthLimit) : ImagesCapture{loop},
            fileId{0}, nextImgId{0}, initialImageId{initialImageId}, readLengthLimit{readLengthLimit}, input{input} {
        DIR *dir = opendir(input.c_str());
        if (!dir)
            throw InvalidInput("Can't find the dir by " + input);
        while (struct dirent *ent = readdir(dir))
            if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, ".."))
                names.emplace_back(ent->d_name);
        closedir(dir);
        if (names.empty())
            throw OpenError("The dir " + input + " is empty");
        sort(names.begin(), names.end());
        size_t readImgs = 0;
        while (fileId < names.size()) {
            cv::Mat img = cv::imread(input + '/' + names[fileId]);
            if (img.data) {
                ++readImgs;
                if (readImgs - 1 >= initialImageId) return;
            }
            ++fileId;
        }
        throw OpenError("Can't read the first image from " + input);
    }

    double fps() const override {return 1.0;}

    std::string getType() const override {return "DIR";}

    cv::Mat read() override {
        auto startTime = std::chrono::steady_clock::now();

        while (fileId < names.size() && nextImgId < readLengthLimit) {
            cv::Mat img = cv::imread(input + '/' + names[fileId]);
            slog::info << "DirReader: image {" << names[fileId] << "}, " << img.cols << "x" << img.rows << slog::endl;
            ++fileId;
            if (img.data) {
                ++nextImgId;
                readerMetrics.update(startTime);
                return img;
            }
        }

        if (loop) {
            fileId = 0;
            size_t readImgs = 0;
            while (fileId < names.size()) {
                cv::Mat img = cv::imread(input + '/' + names[fileId]);
                ++fileId;
                if (img.data) {
                    ++readImgs;
                    if (readImgs - 1 >= initialImageId) {
                        nextImgId = 1;
                        readerMetrics.update(startTime);
                        return img;
                    }
                }
            }
        }
        return cv::Mat{};
    }
};

class VideoCapWrapper : public ImagesCapture {
    cv::VideoCapture cap;
    size_t nextImgId;
    const double initialImageId;
    size_t readLengthLimit;

public:
    VideoCapWrapper(const std::string &input, bool loop, size_t initialImageId, size_t readLengthLimit)
            : ImagesCapture{loop}, nextImgId{0}, initialImageId{static_cast<double>(initialImageId)} {

        if (cap.open(input)) {
            this->readLengthLimit = readLengthLimit;
            if (!cap.set(cv::CAP_PROP_POS_FRAMES, this->initialImageId))
                throw OpenError("Can't set the frame to begin with");
            return;
        }
        throw InvalidInput("Can't open the video from " + input);
    }

    double fps() const override {return cap.get(cv::CAP_PROP_FPS);}

    std::string getType() const override {return "VIDEO";}

    cv::Mat read() override {
        auto startTime = std::chrono::steady_clock::now();

        if (nextImgId >= readLengthLimit) {
            if (loop && cap.set(cv::CAP_PROP_POS_FRAMES, initialImageId)) {
                nextImgId = 1;
                cv::Mat img;
                cap.read(img);
                readerMetrics.update(startTime);
                return img;
            }
            return cv::Mat{};
        }
        cv::Mat img;
        if (!cap.read(img) && loop && cap.set(cv::CAP_PROP_POS_FRAMES, initialImageId)) {
            nextImgId = 1;
            cap.read(img);
        } else {
            ++nextImgId;
        }
        readerMetrics.update(startTime);
        return img;
    }
};

class CameraCapWrapper : public ImagesCapture {
    cv::VideoCapture cap;
    size_t nextImgId;
    const double initialImageId;
    size_t readLengthLimit;

public:
    CameraCapWrapper(const std::string &input, bool loop, size_t initialImageId, size_t readLengthLimit,
                cv::Size cameraResolution)
            : ImagesCapture{loop}, nextImgId{0}, initialImageId{static_cast<double>(initialImageId)} {

        try {
            if (cap.open(std::stoi(input))) {
                this->readLengthLimit = loop ? std::numeric_limits<size_t>::max() : readLengthLimit;
                cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
                cap.set(cv::CAP_PROP_FRAME_WIDTH, cameraResolution.width);
                cap.set(cv::CAP_PROP_FRAME_HEIGHT, cameraResolution.height);
                cap.set(cv::CAP_PROP_AUTOFOCUS, true);
                cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
                return;
            }
            throw OpenError("Can't open the camera from " + input);
        }
        catch (const std::invalid_argument&) { throw InvalidInput("Can't find the camera " + input); }
        catch (const std::out_of_range&) { throw InvalidInput("Can't find the camera " + input); }
    }

    double fps() const override {return cap.get(cv::CAP_PROP_FPS) > 0 ? cap.get(cv::CAP_PROP_FPS) : 30;}

    std::string getType() const override {return "CAMERA";}

    cv::Mat read() override {
        auto startTime = std::chrono::steady_clock::now();

        if (nextImgId >= readLengthLimit) {
            return cv::Mat{};
        }
        cv::Mat img;
        if (!cap.read(img)) {
            throw std::runtime_error("The image can't be captured from the camera");
        }
        ++nextImgId;

        readerMetrics.update(startTime);
        return img;
    }
};

std::unique_ptr<ImagesCapture> openImagesCapture(const std::string &input, bool loop, size_t initialImageId,
        size_t readLengthLimit, cv::Size cameraResolution) {
    if (readLengthLimit == 0) throw std::runtime_error{"Read length limit must be positive"};
    std::vector<std::string> invalidInputs, openErrors;
    try { return std::unique_ptr<ImagesCapture>(new ImreadWrapper{input, loop}); }
    catch (const InvalidInput& e) { invalidInputs.push_back(e.what()); }
    catch (const OpenError& e) { openErrors.push_back(e.what()); }

    try { return std::unique_ptr<ImagesCapture>(new DirReader{input, loop, initialImageId, readLengthLimit}); }
    catch (const InvalidInput& e) { invalidInputs.push_back(e.what()); }
    catch (const OpenError& e) { openErrors.push_back(e.what()); }

    try { return std::unique_ptr<ImagesCapture>(new VideoCapWrapper{input, loop, initialImageId, readLengthLimit}); }
    catch (const InvalidInput& e) { invalidInputs.push_back(e.what()); }
    catch (const OpenError& e) { openErrors.push_back(e.what()); }

    try { return std::unique_ptr<ImagesCapture>(new CameraCapWrapper{input, loop, initialImageId, readLengthLimit, cameraResolution}); }
    catch (const InvalidInput& e) { invalidInputs.push_back(e.what()); }
    catch (const OpenError& e) { openErrors.push_back(e.what()); }

    std::vector<std::string> errorMessages = openErrors.empty() ? invalidInputs : openErrors;
    std::string errorsInfo;
    for (const auto& message: errorMessages) {
        errorsInfo.append(message + "\n");
    }
    throw std::runtime_error(errorsInfo);
}
