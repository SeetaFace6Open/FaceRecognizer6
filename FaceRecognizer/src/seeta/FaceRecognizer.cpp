#include "seeta/FaceRecognizer.h"
#include "seeta/common_alignment.h"

#include <orz/utils/log.h>
#include <api/cpp/tensorstack.h>
#include <api/cpp/module.h>

#include <orz/io/jug/jug.h>
#include <orz/io/i.h>
#include <orz/io/dir.h>
#include <orz/codec/json.h>
#include <fstream>
#include <cfloat>
#include <cmath>

#include "FaceAlignment.h"

#ifdef SEETA_MODEL_ENCRYPT
#include "SeetaLANLock.h"
#include "hidden/SeetaLockFunction.h"
#include "hidden/SeetaLockVerifyLAN.h"
#endif

#include "orz/io/stream/filestream.h"
#include "model_helper.h"

namespace seeta {
    namespace v6 {
        using namespace ts::api;

        static std::string read_txt_file(std::ifstream &in) {
            std::ostringstream tmp;
            tmp << in.rdbuf();
            return tmp.str();
        }
        static orz::jug read_jug_from_json_or_sta(const std::string &filename) {
            std::ifstream ifs(filename, std::ios::binary);

            if (!ifs.is_open()) {
                ORZ_LOG(orz::ERROR) << "Can not access: " << filename << orz::crash;
            }

            int32_t mark;
            ifs.read(reinterpret_cast<char*>(&mark), 4);

            orz::jug model;

            try {
                if (mark == orz::STA_MASK) {
                    model = orz::jug_read(ifs);
                } else {
                    ifs.seekg(0, std::ios::beg);
                    std::string json = read_txt_file(ifs);
                    model = orz::json2jug(json, filename);

                }
            } catch (const orz::Exception &) {
                ORZ_LOG(orz::ERROR) << "Model must be sta or json file, given: " << filename << orz::crash;
                return orz::jug();
            }

            if (model.invalid()) {
                ORZ_LOG(orz::ERROR) << "File format error: " << filename << orz::crash;
        }

            return model;
        }

        static Module parse_tsm_module(const orz::jug &model, const std::string &root) {
            if (model.valid(orz::Piece::BINARY)) {
                auto binary = model.to_binary();
                BufferReader reader(binary.data(), binary.size());
                return Module::Load(reader);
            } else if (model.valid(orz::Piece::STRING)) {
                auto commands = orz::Split(model.to_string(), '@', 3);
                if (commands.size() != 3 || !commands[0].empty() || commands[1] != "file") {
                    ORZ_LOG(orz::ERROR) << R"(Model: /backbone/tsm must be "@file@..." or "@binary@...")" << orz::crash;
                }
                std::string path = root.empty() ? commands[2] : orz::Join({root, commands[2]}, orz::FileSeparator());
                return Module::Load(path);
            } else {
                ORZ_LOG(orz::ERROR) << R"(Model: /backbone/tsm must be "@file@..." or "@binary@...")" << orz::crash;
            }
            return Module();
        }

        struct ModelParam {
            ModelParam() = default;

            struct {
                std::string version = "single";
                int height = 256;
                int width = 256;
                int channels = 3;
            } alignment;

            std::vector<orz::jug> pre_processor;

            struct {
                orz::jug tsm;
            } backbone;

            struct {
                bool normalize = true;
                int sqrt_times = 0;
            } post_processor;

            struct {
                float threshold = 0.05;
                struct {
                    std::string format = "HWC";
                    int height = 256;
                    int width = 256;
                    int channels = 3;
                } input;
                struct {
                    int size = 256;
                } output;
                orz::jug compare;   // an op like {"op": "dot"}, could be invalid
                orz::jug similarity;    // an op like {"op": "sigmoid", "params": [3.0, 7.0]} or {"op", "none"}, could be invalid
            } global;

            static bool to_bool(const orz::jug &jug) {
                return jug.to_bool();
            }
            static std::vector<int> to_int_list(const orz::jug &jug) {
                if (jug.invalid(orz::Piece::LIST)) throw orz::Exception("jug must be list");
                std::vector<int> list(jug.size());
                for (size_t i = 0; i < list.size(); ++i) {
                    list[i] = jug[i].to_int();
                }
                return std::move(list);
            }
            static std::vector<std::vector<int>> to_int_list_list(const orz::jug &jug) {
                if (jug.invalid(orz::Piece::LIST)) throw orz::Exception("jug must be list");
                std::vector<std::vector<int>> list(jug.size());
                for (size_t i = 0; i < list.size(); ++i) {
                    list[i] = to_int_list(jug[i]);
                }
                return std::move(list);
            }
            static std::vector<float> to_float_list(const orz::jug &jug) {
                if (jug.invalid(orz::Piece::LIST)) throw orz::Exception("jug must be list");
                std::vector<float> list(jug.size());
                for (size_t i = 0; i < list.size(); ++i) {
                    list[i] = jug[i].to_float();
                }
                return std::move(list);
            }
            static std::vector<std::vector<float>> to_float_list_list(const orz::jug &jug) {
                if (jug.invalid(orz::Piece::LIST)) throw orz::Exception("jug must be list");
                std::vector<std::vector<float>> list(jug.size());
                for (size_t i = 0; i < list.size(); ++i) {
                    list[i] = to_float_list(jug[i]);
                }
                return std::move(list);
            }
        };

        class CompareEngine {
        public:
            using self = CompareEngine;
            using shared = std::shared_ptr<self>;

            virtual float compare(const float *lhs, const float *rhs, int size) = 0;

            static shared Load(const orz::jug &jug);
        };

        class CompareDot : public CompareEngine {
        public:
            using self = CompareDot;
            using supper = CompareEngine;
            using shared = std::shared_ptr<self>;

            float compare(const float *lhs, const float *rhs, int size) final {
                float sum = 0;
                for (int i = 0; i < size; ++i) {
                    sum += *lhs * *rhs;
                    ++lhs;
                    ++rhs;
                }
                return sum;
            }
        };

        CompareEngine::shared CompareEngine::Load(const orz::jug &jug) {
            if (jug.invalid(orz::Piece::DICT)) {
                ORZ_LOG(orz::ERROR) << "Model: /global/compare must be dict" << orz::crash;
            }
            auto op = orz::jug_get<std::string>(jug["op"], "");
            if (op.empty()) {
                ORZ_LOG(orz::ERROR) << R"(Model: /global/compare should be set like {"op": "dot"}.)" << orz::crash;
            }
            if (op == "dot") {
                return std::make_shared<CompareDot>();
            } else {
                ORZ_LOG(orz::ERROR) << "Model: /global/compare \"" << jug << "\" not supported." << orz::crash;
            }
            return nullptr;
        }

        class SimilarityEngine {
        public:
            using self = SimilarityEngine;
            using shared = std::shared_ptr<self>;

            virtual float similarity(float x) = 0;

            static shared Load(const orz::jug &jug);
        };

        class SimilarityNone : public SimilarityEngine {
        public:
            using self = SimilarityNone;
            using supper = SimilarityEngine;
            using shared = std::shared_ptr<self>;

            float similarity(float x) final { return std::max<float>(x, 0); }
        };

        class SimilaritySigmoid : public SimilarityEngine {
        public:
            using self = SimilaritySigmoid;
            using supper = SimilarityEngine;
            using shared = std::shared_ptr<self>;

            SimilaritySigmoid(float a = 0, float b = 1)
                : m_a(a), m_b(b) {}

            float similarity(float x) final {
                return 1 / (1 + std::exp(m_a - m_b * std::max<float>(x, 0)));
            }

        private:
            float m_a;
            float m_b;
        };

        SimilarityEngine::shared SimilarityEngine::Load(const orz::jug &jug) {
            if (jug.invalid(orz::Piece::DICT)) {
                ORZ_LOG(orz::ERROR) << "Model: /global/similarity must be dict" << orz::crash;
            }
            auto op = orz::jug_get<std::string>(jug["op"], "");
            if (op.empty()) {
                ORZ_LOG(orz::ERROR) << R"(Model: /global/similarity should be set like {"op": "none"}.)" << orz::crash;
            }
            if (op == "none") {
                return std::make_shared<SimilarityNone>();
            } else if (op == "sigmoid") {
                std::vector<float> params;
                try {
                    params = ModelParam::to_float_list(jug["params"]);
                } catch (...) {}
                if (params.size() != 2) {
                    ORZ_LOG(orz::ERROR) << R"(Model: /global/similarity "sigmoid" must set "params" like "{"op": "sigmoid", "params": [0, 1]}")" << orz::crash;
                }
                return std::make_shared<SimilaritySigmoid>(params[0], params[1]);
            } else {
                ORZ_LOG(orz::ERROR) << "Model: /global/similarity \"" << jug << "\" not supported." << orz::crash;
            }
            return nullptr;
        }

        ModelParam parse_model(const orz::jug &model) {
            ModelParam param;

            if (model.invalid(orz::Piece::DICT)) ORZ_LOG(orz::ERROR) << "Model: / must be dict" << orz::crash;

            auto pre_processor = model["pre_processor"];
            auto backbone = model["backbone"];
            auto post_processor = model["post_processor"];
            auto global = model["global"];
            auto alignment = model["alignment"];

            if (alignment.valid()) {
                if (alignment.valid(orz::Piece::DICT)) {
                    auto version = orz::jug_get<std::string>(alignment["version"], "single");
                    param.alignment.version = version;
                    param.alignment.width = orz::jug_get<int>(alignment["width"], param.alignment.width);
                    param.alignment.height = orz::jug_get<int>(alignment["height"], param.alignment.height);
                    param.alignment.channels = orz::jug_get<int>(alignment["channels"], param.alignment.channels);
                } else {
                    ORZ_LOG(orz::ERROR) << "Model: /alignment must be dict" << orz::crash;
                }
            }

            if (pre_processor.valid()) {
                if (pre_processor.valid(orz::Piece::LIST)) {
                    auto size = pre_processor.size();
                    for (decltype(size) i = 0; i < size; ++i) {
                        param.pre_processor.emplace_back(pre_processor[i]);
                    }
                } else {
                    ORZ_LOG(orz::ERROR) << "Model: /pre_processor must be list" << orz::crash;
                }
            }

            if (backbone.valid(orz::Piece::DICT)) {
                auto tsm = backbone["tsm"];
                if (tsm.invalid()) {
                    ORZ_LOG(orz::ERROR) << R"(Model: /backbone/tsm must be "@file@..." or "@binary@...")" << orz::crash;
                }
                param.backbone.tsm = tsm;
            } else {
                ORZ_LOG(orz::ERROR) << "Model: /backbone must be dict" << orz::crash;
            }

            if (post_processor.valid()) {
                if (post_processor.valid(orz::Piece::DICT)) {
                    param.post_processor.normalize = orz::jug_get<bool>(post_processor["normalize"], true);
                    if (!param.post_processor.normalize) {
                        ORZ_LOG(orz::ERROR) << "Model: /post_processor/normalize must be true" << orz::crash;
                    }
                    param.post_processor.sqrt_times = orz::jug_get<int>(post_processor["sqrt_times"], param.post_processor.sqrt_times);
                } else {
                    ORZ_LOG(orz::ERROR) << "Model: /post_processor must be dict" << orz::crash;
                }
            }

            if (global.valid(orz::Piece::DICT)) {
                param.global.threshold = orz::jug_get<float>(global["threshold"], param.global.threshold);
                auto input = global["input"];
                if (input.invalid(orz::Piece::DICT)) ORZ_LOG(orz::ERROR) << "Model: /global/input must be dict" << orz::crash;
                auto output = global["output"];
                if (output.invalid(orz::Piece::DICT)) ORZ_LOG(orz::ERROR) << "Model: /global/output must be dict" << orz::crash;
                auto compare = global["compare"];
                if (compare.invalid(orz::Piece::DICT)) ORZ_LOG(orz::ERROR) << "Model: /global/compare must be dict" << orz::crash;
                auto similarity = global["similarity"];
                if (similarity.invalid(orz::Piece::DICT)) ORZ_LOG(orz::ERROR) << "Model: /global/similarity must be dict" << orz::crash;

                decltype(param.global.input) param_input;
                param_input.format = orz::jug_get<std::string>(input["format"], param_input.format);
                param_input.height = orz::jug_get<int>(input["height"], param_input.height);
                param_input.width = orz::jug_get<int>(input["width"], param_input.width);
                param_input.channels = orz::jug_get<int>(input["channels"], param_input.channels);
                param.global.input = param_input;

                // if (param_input.format != "HWC" || param_input.height != 256 || param_input.width != 256 || param_input.channels != 3) {
                //     ORZ_LOG(orz::ERROR) << "Model: /global/input must be " << R"({"format": "HWC", "width": 256, "height": 256, "channels": 3})" << orz::crash;
                // }

                auto param_output_size = output["size"];
                param.global.output.size = orz::jug_get(output["size"], 0);
                if (param.global.output.size <= 0) {
                    ORZ_LOG(orz::ERROR) << "Model: /global/output/size must greater than 0" << orz::crash;
                }

                param.global.compare = compare;
                param.global.similarity = similarity;
            } else {
                ORZ_LOG(orz::ERROR) << "Model: /global must be dict" << orz::crash;
            }

            return param;
        }

        Device to_ts_device(const seeta::ModelSetting &setting) {
            switch (setting.get_device()) {
                case seeta::ModelSetting::Device::AUTO:
                    return Device("cpu");
                case seeta::ModelSetting::Device::CPU:
                    return Device("cpu");
                case seeta::ModelSetting::Device::GPU:
                    return Device("gpu", setting.id);
                default:
                    return Device("cpu");
            }
        }

        static void build_filter(ImageFilter &filter, const std::vector<orz::jug> &pre_processor) {
            filter.clear();
            for (size_t i = 0; i < pre_processor.size(); ++i) {
                auto &processor = pre_processor[i];
                if (processor.invalid(orz::Piece::DICT)) {
                    ORZ_LOG(orz::ERROR) << "Model: the " << i << "-th processor \"" << processor << "\" should be dict" << orz::crash;
                }
                auto op = orz::jug_get<std::string>(processor["op"], "");
                if (op.empty()) {
                    ORZ_LOG(orz::ERROR) << R"(Model: processor should be set like {"op": "to_float"}.)" << orz::crash;
                }
                if (op == "to_float") {
                    filter.to_float();
                } else if (op == "to_chw") {
                    filter.to_chw();
                } else if (op == "scale") {
                    float scale = FLT_MAX;
                    scale = orz::jug_get<float>(processor["scale"], scale);
                    if (scale == FLT_MAX) {
                        ORZ_LOG(orz::ERROR) << R"(Model: processor "scale" must set "scale" like "{"op": "scale", "scale": 0.0039}")" << orz::crash;
                    }
                    filter.scale(scale);
                } else if (op == "sub_mean") {
                    std::vector<float> mean;
                    try {
                        mean = ModelParam::to_float_list(processor["mean"]);
                    } catch (...) {}
                    if (mean.empty()) {
                        ORZ_LOG(orz::ERROR) << R"(Model: processor "sub_mean" must set "mean" like "{"op": "sub_mean", "mean": [104, 117, 123]}")" << orz::crash;
                    }
                    filter.sub_mean(mean);
                }  else if (op == "div_std") {
                    std::vector<float> std_value;
                    try {
                        std_value = ModelParam::to_float_list(processor["std"]);
                    } catch (...) {}
                    if (std_value.empty()) {
                        ORZ_LOG(orz::ERROR) << R"(Model: processor "div_std" must set "mean" like "{"op": "div_std", "std": [128, 128, 128]}")" << orz::crash;
                    }
                    filter.div_std(std_value);
                } else if (op == "center_crop") {
                    std::vector<int> size;
                    try {
                        size = ModelParam::to_int_list(processor["size"]);
                    } catch (...) {}
                    if (size.empty()) {
                        ORZ_LOG(orz::ERROR) << R"(Model: processor "center_crop" must set "mean" like "{"op": "center_crop", "size": [248, 248]}")" << orz::crash;
                    }
                    if (size.size() == 1) {
                        filter.center_crop(size[0]);
                    } else {
                        filter.center_crop(size[0], size[1]);
                    }
                } else if (op == "resize") {
                    std::vector<int> size;
                    try {
                        size = ModelParam::to_int_list(processor["size"]);
                    } catch (...) {}
                    if (size.empty()) {
                        ORZ_LOG(orz::ERROR) << R"(Model: processor "resize" must set "mean" like "{"op": "resize", "size": [248, 248]}")" << orz::crash;
                    }
                    if (size.size() == 1) {
                        filter.resize(size[0]);
                    } else {
                        filter.resize(size[0], size[1]);
                    }
                } else if (op == "prewhiten") {
                    filter.prewhiten();
                }  else if (op == "channel_swap") {
                    std::vector<int> shuffle;
                    try {
                        shuffle = ModelParam::to_int_list(processor["shuffle"]);
                    } catch (...) {}
                    if (shuffle.size() != 3) {
                        ORZ_LOG(orz::ERROR) << R"(Model: processor "resize" must set "mean" like "{"op": "channel_swap", "shuffle": [2, 1, 0]}")" << orz::crash;
                    }
                    filter.channel_swap(shuffle);
                } else {
                    ORZ_LOG(orz::ERROR) << "Model: processor \"" << processor << "\" not supported." << orz::crash;
                }
            }
        }

        static std::string to_string(const std::vector<int> &shape) {
            std::ostringstream oss;
            oss << "[";
            for (size_t i = 0; i < shape.size(); ++i) {
                if (i) oss << ", ";
                oss << shape[i];
            }
            oss << "]";
            return oss.str();
            (void)(to_string);
        }

        class FaceRecognizer::Implement {
        public:
            Implement(const seeta::ModelSetting &setting);

            Implement(const Implement &other);

            bool ExtractCroppedFace(const SeetaImageData &image, float *features) const;

            float CalculateSimilarity(const float *features1, const float *features2) const;

            bool CropFace(const SeetaImageData &image, const SeetaPointF *points, SeetaImageData &face);


            int get_cpu_affinity() const {
                return m_cpu_affinity;
            }

            void set_cpu_affinity(int level) {
                switch (level) {
                    case 0:
                        m_bench.set_cpu_mode(CpuPowerMode::BIG_CORE);
                        break;
                    case 1:
                        m_bench.set_cpu_mode(CpuPowerMode::LITTLE_CORE);
                        break;
                    case 2:
                        m_bench.set_cpu_mode(CpuPowerMode::BALANCE);
                        break;
                    default:
                        level = -1;
                        break;
                }
                m_cpu_affinity = level;
            }
            void set(FaceRecognizer::Property property, double value) {
                switch (property) {
                    default:
                        break;
                    case FaceRecognizer::PROPERTY_NUMBER_THREADS:
                    {
                        if (value < 1) value = 1;
                        auto threads = int(value);
                        m_number_threads = threads;
                        m_bench.set_computing_thread_number(threads);
                        break;
                    }

                    case FaceRecognizer::PROPERTY_ARM_CPU_MODE:
                    {
                        set_cpu_affinity(int32_t(value));
                        break;
                    }

                }
            }

            double get(FaceRecognizer::Property property) const {
                switch (property) {
                    default:
                        return 0;
                    case FaceRecognizer::PROPERTY_NUMBER_THREADS:
                        return m_number_threads;
                    case FaceRecognizer::PROPERTY_ARM_CPU_MODE:
                        return get_cpu_affinity();

                }
            }

        public:
            ModelParam m_param;
            mutable Workbench m_bench;

            SimilarityEngine::shared m_similarity;
            CompareEngine::shared m_compare;
            FaceAlignment::shared m_alignment;

            int32_t m_number_threads = 4;
            int m_cpu_affinity = -1;

        };

        FaceRecognizer::Implement::Implement(const seeta::ModelSetting &setting) {
            auto &model = setting.get_model();
            if (model.size() != 1) {
                ORZ_LOG(orz::ERROR) << "Must have 1 model." << orz::crash;
            }
			
			auto jug = get_model_jug(model[0].c_str());

            auto param = parse_model(jug);
            // check parameter
            if (param.alignment.version != "single" &&
                param.alignment.version != "multi" &&
                param.alignment.version != "arcface") {
                ORZ_LOG(orz::ERROR) << "Not supported alignment version: " << param.alignment.version << orz::crash;
            }

            // parse tsm module
            std::string root = orz::cut_path_tail(model[0]);
            auto tsm = parse_tsm_module(param.backbone.tsm, root);
            // add image filter
            auto device = to_ts_device(setting);
            auto bench = Workbench::Load(tsm, device);
            // ts_Workbench_setup_device(bench.get_raw());
            ImageFilter filter(device);

            build_filter(filter, param.pre_processor);
            bench.bind_filter(0, filter);

            this->m_compare = CompareEngine::Load(param.global.compare);
            this->m_similarity = SimilarityEngine::Load(param.global.similarity);
            this->m_alignment = std::make_shared<FaceAlignment>(
                    param.alignment.version,
                    param.alignment.width, param.alignment.height,
                    5);

            this->m_param = param;
            this->m_bench = bench;
        }

        static void normalize(float *features, int num)
        {
            double norm = 0;
            float *dim = features;
            for (int i = 0; i < num; ++i)
            {
                norm += *dim * *dim;
                ++dim;
            }
            norm = std::sqrt(norm) + 1e-5;
            dim = features;
            for (int i = 0; i < num; ++i)
            {
                *dim /= float(norm);
                ++dim;
            }
        }


        bool FaceRecognizer::Implement::ExtractCroppedFace(const SeetaImageData &image, float *features) const {
            if (image.height != m_param.global.input.height ||
                image.width != m_param.global.input.width ||
                image.channels != m_param.global.input.channels)
                return false;

            // ts_Workbench_setup_device(m_bench.get_raw());

            auto tensor = tensor::build(UINT8, {1, image.height, image.width, image.channels}, image.data);
            m_bench.input(0, tensor);
            m_bench.run();
            auto output = tensor::cast(FLOAT32, m_bench.output(0));
            auto output_size = m_param.global.output.size;
            if (output.count() != output_size) {
                ORZ_LOG(orz::ERROR) << "Extracted features size must be "
                                    << output_size << " vs. " << output.count() << " given.";
                return false;
            }
            std::memcpy(features, output.data(), output_size * sizeof(float));

            if (m_param.post_processor.sqrt_times > 0) {
                auto times = m_param.post_processor.sqrt_times;
                while (times--) {
                    for (int i = 0; i < output_size; ++i) {
                        features[i] = std::sqrt(features[i]);
                    }
                }
            }

            if (m_param.post_processor.normalize) {
                normalize(features, output_size);
            }

            return true;
        }

        float FaceRecognizer::Implement::CalculateSimilarity(const float *features1, const float *features2) const {
            if (features1 == nullptr || features2 == nullptr) return 0;
            auto similarity = m_compare->compare(features1, features2, m_param.global.output.size);
            return m_similarity->similarity(similarity);
        }

        bool FaceRecognizer::Implement::CropFace(const SeetaImageData &image, const SeetaPointF *points,
                                                 SeetaImageData &face) {
            if (m_alignment->crop_width() != face.width || m_alignment->crop_height() != face.height || image.channels != face.channels) {
                ORZ_LOG(orz::ERROR) << "Crop face image data shape must be ["
                                    << m_alignment->crop_width() << ", " << m_alignment->crop_height() << ", "
                                    << image.channels << "], got ["
                                    << face.width << ", " << face.height << ", "
                                    << face.channels << "]." << orz::crash;
                return false;
    }
            m_bench.setup_context();
            m_alignment->crop_face(image, points, face);
            return true;
        }

        FaceRecognizer::Implement::Implement(const FaceRecognizer::Implement &other) {
            *this = other;
            this->m_bench = this->m_bench.clone();
            this->m_alignment = this->m_alignment->clone();
        }
    }

    FaceRecognizer::FaceRecognizer(const SeetaModelSetting &setting)
        : m_impl(new Implement(setting)) {

    }

    FaceRecognizer::~FaceRecognizer() {
        delete m_impl;
    }

    int FaceRecognizer::GetCropFaceWidth() {
        ORZ_LOG(orz::INFO) << "Using not recommended API GetCropFaceWidth, please use GetCropFaceWidthV2 instead.";
        return 256;
    }

    int FaceRecognizer::GetCropFaceHeight() {
        ORZ_LOG(orz::INFO) << "Using not recommended API GetCropFaceHeight, please use GetCropFaceHeightV2 instead.";
        return 256;
    }

    int FaceRecognizer::GetCropFaceChannels() {
        ORZ_LOG(orz::INFO) << "Using not recommended API GetCropFaceChannels, please use GetCropFaceChannelsV2 instead.";
        return 3;
    }

    int FaceRecognizer::GetExtractFeatureSize() const {
        return m_impl->m_param.global.output.size;
    }

    bool FaceRecognizer::ExtractCroppedFace(const SeetaImageData &image, float *features) const {
        return m_impl->ExtractCroppedFace(image, features);
    }

    float FaceRecognizer::CalculateSimilarity(const float *features1, const float *features2) const {
        return m_impl->CalculateSimilarity(features1, features2);
    }

    bool FaceRecognizer::Extract(const SeetaImageData &image, const SeetaPointF *points, float *features) const {
        seeta::ImageData cropped_face(m_impl->m_alignment->crop_width(), m_impl->m_alignment->crop_height(), m_impl->m_param.alignment.channels);
        if (!m_impl->CropFace(image, points, cropped_face)) return false;
        return m_impl->ExtractCroppedFace(cropped_face, features);
    }

    bool FaceRecognizer::CropFace(const SeetaImageData &image, const SeetaPointF *points, SeetaImageData &face) {
        ORZ_LOG(orz::INFO) << "Using not recommended API CropFace, please use CropFaceV2 instead.";
        if (face.height != 256 || face.width != 256 || face.channels != 3) return false;
        static const float mean_shape[10] = {
                89.3095f, 72.9025f,
                169.3095f, 72.9025f,
                127.8949f, 127.0441f,
                96.8796f, 184.8907f,
                159.1065f, 184.7601f,
        };
        float landmarks[10] = {
                float(points[0].x), float(points[0].y),
                float(points[1].x), float(points[1].y),
                float(points[2].x), float(points[2].y),
                float(points[3].x), float(points[3].y),
                float(points[4].x), float(points[4].y),
        };
        face_crop_core(image.data, image.width, image.height, image.channels, face.data, 256, 256, landmarks, 5, mean_shape, 256, 256);

        return true;
    }

    int FaceRecognizer::GetCropFaceWidthV2() const {
        return m_impl->m_alignment->crop_width();
    }

    int FaceRecognizer::GetCropFaceHeightV2() const {
        return m_impl->m_alignment->crop_height();
    }

    int FaceRecognizer::GetCropFaceChannelsV2() const {
        return m_impl->m_param.alignment.channels;
    }

    bool FaceRecognizer::CropFaceV2(const SeetaImageData &image, const SeetaPointF *points, SeetaImageData &face) {
        return m_impl->CropFace(image, points, face);
    }

    FaceRecognizer::FaceRecognizer(const FaceRecognizer::self *other)
            : m_impl(nullptr) {
        if (other == nullptr) {
            ORZ_LOG(orz::ERROR) << "Parameter 1 can not be nullptr." << orz::crash;
        }
        m_impl = new Implement(*other->m_impl);
    }


    void FaceRecognizer::set(FaceRecognizer::Property property, double value) {
        m_impl->set(property, value);
    }

    double FaceRecognizer::get(FaceRecognizer::Property property) const {
        return m_impl->get(property);
    }


}
