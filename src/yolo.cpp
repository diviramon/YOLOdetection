#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <queue>
#include <fstream>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>         // std::mutex, std::unique_lock
#include <cmath>

// It makes sense only for video-Camera (not for video-File)
// To use - uncomment the following line. Optical-flow is supported only by OpenCV 3.x - 4.x
#include "yolo_v2_class.hpp"    // imported functions from DLL
#include "yolo.hpp"
#include <opencv2/opencv.hpp>            // C++
#include <opencv2/core/version.hpp>
#include <opencv2/videoio/videoio.hpp>


std::vector<bbox_t> get_3d_coordinates(std::vector<bbox_t> bbox_vect, cv::Mat xyzrgba) {
    return bbox_vect;
}

#define OPENCV_VERSION CVAUX_STR(CV_VERSION_MAJOR)"" CVAUX_STR(CV_VERSION_MINOR)"" CVAUX_STR(CV_VERSION_REVISION)
#ifndef USE_CMAKE_LIBS
#pragma comment(lib, "opencv_world" OPENCV_VERSION ".lib")
#pragma comment(lib, "opencv_cudaoptflow" OPENCV_VERSION ".lib")
#pragma comment(lib, "opencv_cudaimgproc" OPENCV_VERSION ".lib")
#pragma comment(lib, "opencv_core" OPENCV_VERSION ".lib")
#pragma comment(lib, "opencv_imgproc" OPENCV_VERSION ".lib")
#pragma comment(lib, "opencv_highgui" OPENCV_VERSION ".lib")
#endif    // USE_CMAKE_LIBS

void draw_boxes(cv::Mat mat_img, std::vector<bbox_t> result_vec, std::vector<std::string> obj_names,
    int current_det_fps, int current_cap_fps)
{
    int const colors[6][3] = { { 1,0,1 },{ 0,0,1 },{ 0,1,1 },{ 0,1,0 },{ 1,1,0 },{ 1,0,0 } };

    for (auto &i : result_vec) {
        cv::Scalar color = obj_id_to_color(i.obj_id);
        cv::rectangle(mat_img, cv::Rect(i.x, i.y, i.w, i.h), color, 2);
        if (obj_names.size() > i.obj_id) {
            std::string obj_name = obj_names[i.obj_id];
            if (i.track_id > 0) obj_name += " - " + std::to_string(i.track_id);
            cv::Size const text_size = getTextSize(obj_name, cv::FONT_HERSHEY_COMPLEX_SMALL, 1.2, 2, 0);
            int max_width = (text_size.width > i.w + 2) ? text_size.width : (i.w + 2);
            max_width = std::max(max_width, (int)i.w + 2);
            //max_width = std::max(max_width, 283);

            cv::rectangle(mat_img, cv::Point2f(std::max((int)i.x - 1, 0), std::max((int)i.y - 35, 0)),
                cv::Point2f(std::min((int)i.x + max_width, mat_img.cols - 1), std::min((int)i.y, mat_img.rows - 1)),
                color, CV_FILLED, 8, 0);
            putText(mat_img, obj_name, cv::Point2f(i.x, i.y - 16), cv::FONT_HERSHEY_COMPLEX_SMALL, 1.2, cv::Scalar(0, 0, 0), 2);
        }
    }
    if (current_det_fps >= 0 && current_cap_fps >= 0) {
        std::string fps_str = "FPS detection: " + std::to_string(current_det_fps) + "   FPS capture: " + std::to_string(current_cap_fps);
        putText(mat_img, fps_str, cv::Point2f(10, 20), cv::FONT_HERSHEY_COMPLEX_SMALL, 1.2, cv::Scalar(50, 255, 0), 2);
    }
}

void show_console_result(std::vector<bbox_t> const result_vec, std::vector<std::string> const obj_names, int frame_id) {
    if (frame_id >= 0) std::cout << " Frame: " << frame_id << std::endl;
    for (auto &i : result_vec) {
        if (obj_names.size() > i.obj_id) std::cout << obj_names[i.obj_id] << " - ";
        std::cout << "obj_id = " << i.obj_id << ",  x = " << i.x << ", y = " << i.y
            << ", w = " << i.w << ", h = " << i.h
            << std::setprecision(3) << ", prob = " << i.prob << std::endl;
    }
}

std::vector<std::string> objects_names_from_file(std::string const filename) {
    std::ifstream file(filename);
    std::vector<std::string> file_lines;
    if (!file.is_open()) return file_lines;
    for(std::string line; getline(file, line);) file_lines.push_back(line);
    std::cout << "object names loaded \n";
    return file_lines;
}

template <typename T>
send_one_replaceable_object_t<T>::send_one_replaceable_object_t(bool _sync) : sync(_sync), a_ptr(NULL)
{};

template <typename T>
void send_one_replaceable_object_t<T>::send(T const& _obj) {
    T *new_ptr = new T;
    *new_ptr = _obj;
    if (sync) {
        while (a_ptr.load()) std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    std::unique_ptr<T> old_ptr(a_ptr.exchange(new_ptr));
}

template <typename T>
T send_one_replaceable_object_t<T>::receive() {
    std::unique_ptr<T> ptr;
    do {
        while(!a_ptr.load()) std::this_thread::sleep_for(std::chrono::milliseconds(3));
        ptr.reset(a_ptr.exchange(NULL));
    } while (!ptr);
    T obj = *ptr;
    return obj;
}

template <typename T>
bool send_one_replaceable_object_t<T>::is_object_present() {
    return (a_ptr.load() != NULL);
}

bool operator <(const bbox_t& x, const bbox_t& y){
        return x.track_id < y.track_id;
}

bool check_key(std::unordered_map<unsigned int,unsigned int> m, int key){
    if (m.find(key) == m.end()) return false;
    return true;
}

std::unordered_map<unsigned int, unsigned int> populate_map(std::vector<bbox_t> vec){
    std::unordered_map<unsigned int,unsigned int> m;
    for(auto &i : vec){
        if(check_key(m, i.obj_id)){
            if (i.track_id > m[i.obj_id]){
                m[i.obj_id] = i.track_id;
            }
        }
        else{
            m[i.obj_id] = i.track_id;
        }
    }
    return m;
}

float compute_difference(bbox_t a, bbox_t b){
    int diffx = a.x - b.x;
    int diffy = a.y - b.y;
    float distance = sqrt(pow(diffx,2) + pow(diffy,2));
    return distance;
}

bbox_t find_new_object(std::vector<bbox_t> objects, std::vector<bbox_t> scene, unsigned int obj_id){
    std::map<bbox_t, float> results;
    std::vector<bbox_t> candidates;
    bbox_t result;
    int niter = 0;
    for (auto m : objects){
        if (m.obj_id == obj_id){
            niter++;
            results.insert({m,500.00f});
            candidates.push_back(m);
        }
    }

    for (auto &i : scene){
        if (i.obj_id == obj_id){
            for (auto &j : candidates){
                float diff = compute_difference(i,j);
                if (diff < results[j]) results[j] = diff;
            }
        }
    }

    float max_diff = 0.0;
    for (auto &o : results){
        if (o.second > max_diff) {
            result = o.first;
            max_diff = o.second;
        }
    }
    result.track_id = 1;
    return result;
}

std::vector<bbox_t> analyze_scene(std::unordered_map<unsigned int, unsigned int> m, std::vector<bbox_t> v, std::vector<bbox_t> scene){
    std::vector<bbox_t> results;
    for(auto &i : v){
        if(check_key(m, i.obj_id)){
            if(i.track_id > m[i.obj_id]){
                results.push_back(find_new_object(v, scene, i.obj_id));
            }
        }
        else{
            results.push_back(i);
        }
    }
    return results;
}

std::vector<bbox_t> analyze_scene_adv(std::unordered_map<unsigned int, unsigned int> m, std::vector<bbox_t> v, std::vector<bbox_t> scene){
    std::vector<bbox_t> results;
    std::vector<unsigned int> checked;
    for(auto &i : v){
        if (!check_key(m,i.obj_id)) results.push_back(i);
        else{
            if (std::find(checked.begin(), checked.end(), i.obj_id) == checked.end())
            {
                results.push_back(find_new_object(v, scene, i.obj_id));
                checked.push_back(i.obj_id);
            }
        }
    }
    return results;
}


int detect(std::string Filename, float const Thresh)
{
    std::string  names_file = "darknet/data/coco.names";
    std::string  cfg_file = "darknet/cfg/yolov3.cfg";
    std::string  weights_file = "darknet/yolov3.weights";
    std::string filename = Filename;
    float thresh = Thresh;

    Detector detector(cfg_file, weights_file);

    auto obj_names = objects_names_from_file(names_file);
    std::string out_videofile = "result.avi";
    bool const save_output_videofile = false;   // true - for history
    bool const send_network = false;        // true - for remote detection
    bool const use_kalman_filter = true;   // true - for stationary camera
    bool detection_sync = false;             // true - for video-file
    Tracker_optflow tracker_flow;




    while (true)
    {
        std::cout << "input image or video filename: ";
        if(filename.size() == 0) std::cin >> filename;
        if (filename.size() == 0) break;

        try {
            preview_boxes_t large_preview(100, 150, false), small_preview(50, 50, true);
            bool show_small_boxes = false;

            std::string const file_ext = filename.substr(filename.find_last_of(".") + 1);
            std::string const protocol = filename.substr(0, 7);
            if (file_ext == "avi" || file_ext == "mp4" || file_ext == "mjpg" || file_ext == "mov" ||     // video file
                protocol == "rtmp://" || protocol == "rtsp://" || protocol == "http://" || protocol == "https:/" ||    // video network stream
                filename == "zed_camera" || file_ext == "svo" || filename == "web_camera" || filename == "webcam")   // ZED stereo camera

            {
                if (protocol == "rtsp://" || protocol == "http://" || protocol == "https:/" || filename == "zed_camera" || filename == "web_camera" || filename == "webcam")
                    detection_sync = false;

                cv::Mat cur_frame;
                std::atomic<int> fps_cap_counter(0), fps_det_counter(0);
                std::atomic<int> current_fps_cap(0), current_fps_det(0);
                std::atomic<bool> exit_flag(false);
                std::atomic<bool> scan_flag(true);
                std::atomic<int> frames_elapsed(0);
                std::unordered_map<unsigned int,unsigned int> ranks;
                std::chrono::steady_clock::time_point steady_start, steady_end;
                int video_fps = 25;
                bool use_zed_camera = false;

                track_kalman_t track_kalman;

                cv::VideoCapture cap;
                if (filename == "web_camera" || filename == "webcam") {
                    cap.open(0);
                    cap >> cur_frame;
                } else if (!use_zed_camera) {
                    cap.open(filename);
                    cap >> cur_frame;
                }
                video_fps = cap.get(cv::CAP_PROP_FPS);
                cv::Size const frame_size = cur_frame.size();
                //cv::Size const frame_size(cap.get(CV_CAP_PROP_FRAME_WIDTH), cap.get(CV_CAP_PROP_FRAME_HEIGHT));
                std::cout << "\n Video size: " << frame_size << std::endl;

                cv::VideoWriter output_video;
                if (save_output_videofile)
                    output_video.open(out_videofile, cv::VideoWriter::fourcc('D', 'I', 'V', 'X'), std::max(35, video_fps), frame_size, true);

                struct detection_data_t {
                    cv::Mat cap_frame;
                    std::shared_ptr<image_t> det_image;
                    std::vector<bbox_t> result_vec;
                    cv::Mat draw_frame;
                    bool new_detection;
                    uint64_t frame_id;
                    bool exit_flag;
                    cv::Mat zed_cloud;
                    std::queue<cv::Mat> track_optflow_queue;
                    detection_data_t() : exit_flag(false), new_detection(false) {}
                };

                const bool sync = detection_sync; // sync data exchange
                send_one_replaceable_object_t<detection_data_t> cap2prepare(sync), cap2draw(sync),
                    prepare2detect(sync), detect2draw(sync), draw2show(sync), draw2write(sync), draw2net(sync);

                std::thread t_cap, t_prepare, t_detect, t_post, t_draw, t_write, t_network;

                // capture new video-frame
                if (t_cap.joinable()) t_cap.join();
                t_cap = std::thread([&]()
                {
                    uint64_t frame_id = 0;
                    detection_data_t detection_data;
                    do {
                        detection_data = detection_data_t();
                        {
                            cap >> detection_data.cap_frame;
                        }
                        fps_cap_counter++;
                        frames_elapsed++;
                        detection_data.frame_id = frame_id++;
                        if (detection_data.cap_frame.empty() || exit_flag) {
                            std::cout << " exit_flag: detection_data.cap_frame.size = " << detection_data.cap_frame.size() << std::endl;
                            detection_data.exit_flag = true;
                            detection_data.cap_frame = cv::Mat(frame_size, CV_8UC3);
                        }

                        if (!detection_sync) {
                            cap2draw.send(detection_data);       // skip detection
                        }
                        cap2prepare.send(detection_data);
                    } while (!detection_data.exit_flag);
                    std::cout << " t_cap exit \n";
                });


                // pre-processing video frame (resize, convertion)
                t_prepare = std::thread([&]()
                {
                    std::shared_ptr<image_t> det_image;
                    detection_data_t detection_data;
                    do {
                        detection_data = cap2prepare.receive();

                        det_image = detector.mat_to_image_resize(detection_data.cap_frame);
                        detection_data.det_image = det_image;
                        prepare2detect.send(detection_data);    // detection

                    } while (!detection_data.exit_flag);
                    std::cout << " t_prepare exit \n";
                });


                // detection by Yolo
                if (t_detect.joinable()) t_detect.join();
                t_detect = std::thread([&]()
                {
                    std::shared_ptr<image_t> det_image;
                    detection_data_t detection_data;
                    do {
                        detection_data = prepare2detect.receive();
                        det_image = detection_data.det_image;
                        std::vector<bbox_t> result_vec;

                        if(det_image)
                            result_vec = detector.detect_resized(*det_image, frame_size.width, frame_size.height, thresh, true);  // true
                        fps_det_counter++;
                        //std::this_thread::sleep_for(std::chrono::milliseconds(150));

                        detection_data.new_detection = true;
                        detection_data.result_vec = result_vec;
                        detect2draw.send(detection_data);
                    } while (!detection_data.exit_flag);
                    std::cout << " t_detect exit \n";
                });

                // draw rectangles (and track objects)
                t_draw = std::thread([&]()
                {
                    std::queue<cv::Mat> track_optflow_queue;
                    detection_data_t detection_data;
                    std::vector<bbox_t> scan_vec; // stores the scene objects
                    std::vector<bbox_t> detect_vec; // stores the new objects that enter the scene
                    do {

                        // for Video-file
                        if (detection_sync) {
                            detection_data = detect2draw.receive();
                        }
                        // for Video-camera
                        else
                        {
                            // get new Detection result if present
                            if (detect2draw.is_object_present()) {
                                cv::Mat old_cap_frame = detection_data.cap_frame;   // use old captured frame
                                detection_data = detect2draw.receive();
                                if (!old_cap_frame.empty()) detection_data.cap_frame = old_cap_frame;
                            }
                            // get new Captured frame
                            else {
                                std::vector<bbox_t> old_result_vec = detection_data.result_vec; // use old detections
                                detection_data = cap2draw.receive();
                                detection_data.result_vec = old_result_vec;
                            }
                        }

                        cv::Mat cap_frame = detection_data.cap_frame;
                        cv::Mat draw_frame = detection_data.cap_frame.clone();
                        std::vector<bbox_t> result_vec = detection_data.result_vec;

                        if (detection_data.new_detection) {
                            tracker_flow.update_tracking_flow(detection_data.cap_frame, detection_data.result_vec);
                            while (track_optflow_queue.size() > 0) {
                                draw_frame = track_optflow_queue.back();
                                result_vec = tracker_flow.tracking_flow(track_optflow_queue.front(), false);
                                track_optflow_queue.pop();
                            }
                        }
                        else {
                            track_optflow_queue.push(cap_frame);
                            result_vec = tracker_flow.tracking_flow(cap_frame, false);
                        }
                        detection_data.new_detection = true;    // to correct kalman filter

                        // track ID by using kalman filter (ideal for stationary cameras)
                        if (use_kalman_filter) {
                            if (detection_data.new_detection) {
                                result_vec = track_kalman.correct(result_vec);
                            }
                            else {
                                std::cout << "Anomaly" << std::endl;
                                result_vec = track_kalman.predict();
                            }
                        }
                        // track ID by using custom function (works better for non stationary cameras)
                        else {
                            int frame_story = std::max(5, current_fps_cap.load());
                            result_vec = detector.tracking_id(result_vec, true, frame_story, 40);
                        }

                        if (!scan_flag){
                            detect_vec = analyze_scene(ranks,result_vec, scan_vec);
                            result_vec = detect_vec;
                        }

                        else scan_vec = result_vec;


                        //small_preview.set(draw_frame, result_vec);
                        //large_preview.set(draw_frame, result_vec);
                        draw_boxes(draw_frame, result_vec, obj_names, current_fps_det, current_fps_cap);
                        //show_console_result(result_vec, obj_names, detection_data.frame_id);
                        //large_preview.draw(draw_frame);
                        //small_preview.draw(draw_frame, true);

                        detection_data.result_vec = result_vec;
                        detection_data.draw_frame = draw_frame;
                        draw2show.send(detection_data);
                        if (send_network) draw2net.send(detection_data);
                        if (output_video.isOpened()) draw2write.send(detection_data);
                    } while (!detection_data.exit_flag);
                    std::cout << " t_draw exit \n";
                });


                // write frame to videofile
                t_write = std::thread([&]()
                {
                    if (output_video.isOpened()) {
                        detection_data_t detection_data;
                        cv::Mat output_frame;
                        do {
                            detection_data = draw2write.receive();
                            if(detection_data.draw_frame.channels() == 4) cv::cvtColor(detection_data.draw_frame, output_frame, CV_RGBA2RGB);
                            else output_frame = detection_data.draw_frame;
                            output_video << output_frame;
                        } while (!detection_data.exit_flag);
                        output_video.release();
                    }
                    std::cout << " t_write exit \n";
                });

                // send detection to the network
                t_network = std::thread([&]()
                {
                    if (send_network) {
                        detection_data_t detection_data;
                        do {
                            detection_data = draw2net.receive();

                            detector.send_json_http(detection_data.result_vec, obj_names, detection_data.frame_id, filename);

                        } while (!detection_data.exit_flag);
                    }
                    std::cout << " t_network exit \n";
                });


                // show detection
                detection_data_t detection_data;
                do {

                    steady_end = std::chrono::steady_clock::now();
                    float time_sec = std::chrono::duration<double>(steady_end - steady_start).count();
                    if (time_sec >= 1) {
                        current_fps_det = fps_det_counter.load() / time_sec;
                        current_fps_cap = fps_cap_counter.load() / time_sec;
                        steady_start = steady_end;
                        fps_det_counter = 0;
                        fps_cap_counter = 0;
                    }

                    detection_data = draw2show.receive();
                    cv::Mat draw_frame = detection_data.draw_frame;

                    //if (extrapolate_flag) {
                    //    cv::putText(draw_frame, "extrapolate", cv::Point2f(10, 40), cv::FONT_HERSHEY_COMPLEX_SMALL, 1.0, cv::Scalar(50, 50, 0), 2);
                    //}

                    cv::imshow("window name", draw_frame);
                    int key = cv::waitKey(3);    // 3 or 16ms
                    if (key == 'f') show_small_boxes = !show_small_boxes;
                    if (key == 'p') while (true) if (cv::waitKey(100) == 'p') break;
                    //if (key == 'e') extrapolate_flag = !extrapolate_flag;
                    if (key == 27) { exit_flag = true;} // press escape to end
                    if (key == 13) {                   // press enter to switch modes: scan (acquiring scene) or detection (show new objects only)
                        scan_flag = !scan_flag;
                        ranks = populate_map(detection_data.result_vec);
                        if (scan_flag) thresh = Thresh;
                        else thresh += 0.1;
                        }

                    //std::cout << " current_fps_det = " << current_fps_det << ", current_fps_cap = " << current_fps_cap << std::endl;
                } while (!detection_data.exit_flag);
                std::cout << " show detection exit \n";

                cv::destroyWindow("window name");
                // wait for all threads
                if (t_cap.joinable()) t_cap.join();
                if (t_prepare.joinable()) t_prepare.join();
                if (t_detect.joinable()) t_detect.join();
                if (t_post.joinable()) t_post.join();
                if (t_draw.joinable()) t_draw.join();
                if (t_write.joinable()) t_write.join();
                if (t_network.joinable()) t_network.join();

                break;

            }
            else if (file_ext == "txt") {    // list of image files
                std::ifstream file(filename);
                if (!file.is_open()) std::cout << "File not found! \n";
                else
                    for (std::string line; file >> line;) {
                        std::cout << line << std::endl;
                        cv::Mat mat_img = cv::imread(line);
                        std::vector<bbox_t> result_vec = detector.detect(mat_img);
                        show_console_result(result_vec, obj_names);
                        //draw_boxes(mat_img, result_vec, obj_names);
                        //cv::imwrite("res_" + line, mat_img);
                    }

            }
            else {    // image file
                cv::Mat mat_img = cv::imread(filename);

                auto start = std::chrono::steady_clock::now();
                std::vector<bbox_t> result_vec = detector.detect(mat_img);
                auto end = std::chrono::steady_clock::now();
                std::chrono::duration<double> spent = end - start;
                std::cout << " Time: " << spent.count() << " sec \n";

                //result_vec = detector.tracking_id(result_vec);    // comment it - if track_id is not required
                draw_boxes(mat_img, result_vec, obj_names);
                cv::imshow("window name", mat_img);
                show_console_result(result_vec, obj_names);
                cv::waitKey(0);
            }
        }
        catch (std::exception &e) { std::cerr << "exception: " << e.what() << "\n"; getchar(); }
        catch (...) { std::cerr << "unknown exception \n"; getchar(); }
        filename.clear();
    }

    return 0;
}
