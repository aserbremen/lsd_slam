/**
 * This file is part of LSD-SLAM.
 *
 * Copyright 2013 Jakob Engel <engelj at in dot tum dot de> (Technical University of Munich)
 * For more information see <http://vision.in.tum.de/lsdslam>
 *
 * LSD-SLAM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * LSD-SLAM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with LSD-SLAM. If not, see <http://www.gnu.org/licenses/>.
 */

#include "LiveSLAMWrapper.h"

#include "SlamSystem.h"
#include "util/globalFuncs.h"
#include "util/settings.h"
#include <boost/thread.hpp>

#include <algorithm>
#include <dirent.h>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>

#include "IOWrapper/ROS/ROSOutput3DWrapper.h"
#include "IOWrapper/ROS/rosReconfigure.h"

#include "util/Undistorter.h"
#include <ros/package.h>

#include "opencv2/opencv.hpp"

std::string &ltrim(std::string &s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
    return s;
}
std::string &rtrim(std::string &s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
    return s;
}
std::string &trim(std::string &s) { return ltrim(rtrim(s)); }

int getdir(std::string dir, std::vector<std::string> &files) {
    DIR *dp;
    struct dirent *dirp;
    if ((dp = opendir(dir.c_str())) == NULL) {
        return -1;
    }

    while ((dirp = readdir(dp)) != NULL) {
        std::string name = std::string(dirp->d_name);

        if (name != "." && name != "..")
            files.push_back(name);
    }
    closedir(dp);

    std::sort(files.begin(), files.end());

    if (dir.at(dir.length() - 1) != '/')
        dir = dir + "/";
    for (unsigned int i = 0; i < files.size(); i++) {
        if (files[i].at(0) != '/')
            files[i] = dir + files[i];
    }

    return files.size();
}

int getFile(std::string source, std::vector<std::string> &files) {
    std::ifstream f(source.c_str());

    if (f.good() && f.is_open()) {
        while (!f.eof()) {
            std::string l;
            std::getline(f, l);

            l = trim(l);

            if (l == "" || l[0] == '#')
                continue;

            files.push_back(l);
        }

        f.close();

        size_t sp = source.find_last_of('/');
        std::string prefix;
        if (sp == std::string::npos)
            prefix = "";
        else
            prefix = source.substr(0, sp);

        for (unsigned int i = 0; i < files.size(); i++) {
            if (files[i].at(0) != '/')
                files[i] = prefix + "/" + files[i];
        }

        return (int)files.size();
    } else {
        f.close();
        return -1;
    }
}

using namespace lsd_slam;
int main(int argc, char **argv) {
    ros::init(argc, argv, "LSD_SLAM");

    dynamic_reconfigure::Server<lsd_slam_core::LSDParamsConfig> srv(ros::NodeHandle("~"));
    srv.setCallback(dynConfCb);

    dynamic_reconfigure::Server<lsd_slam_core::LSDDebugParamsConfig> srvDebug(ros::NodeHandle("~Debug"));
    srvDebug.setCallback(dynConfCbDebug);

    packagePath = ros::package::getPath("lsd_slam_core") + "/";

    // get camera calibration in form of an undistorter object.
    // if no undistortion is required, the undistorter will just pass images through.
    std::string calibFile;
    Undistorter *undistorter = 0;
    if (ros::param::get("~calib", calibFile)) {
        undistorter = Undistorter::getUndistorterForFile(calibFile.c_str());
        ros::param::del("~calib");
    }

    if (undistorter == 0) {
        printf("need camera calibration file! (set using _calib:=FILE)\n");
        exit(0);
    }

    int w = undistorter->getOutputWidth();
    int h = undistorter->getOutputHeight();

    int w_inp = undistorter->getInputWidth();
    int h_inp = undistorter->getInputHeight();

    float fx = undistorter->getK().at<double>(0, 0);
    float fy = undistorter->getK().at<double>(1, 1);
    float cx = undistorter->getK().at<double>(2, 0);
    float cy = undistorter->getK().at<double>(2, 1);
    Sophus::Matrix3f K;
    K << fx, 0.0, cx, 0.0, fy, cy, 0.0, 0.0, 1.0;

    // make output wrapper. just set to zero if no output is required.
    Output3DWrapper *outputWrapper = new ROSOutput3DWrapper(w, h);

    // make slam system
    SlamSystem *system = new SlamSystem(w, h, K, doSlam);
    system->setVisualization(outputWrapper);

    // open image files: first try to open as file.
    std::string source;
    std::vector<std::string> files;
    if (!ros::param::get("~files", source)) {
        printf("need source files! (set using _files:=FOLDER)\n");
        exit(0);
    }
    ros::param::del("~files");

    if (getdir(source, files) >= 0) {
        printf("found %d image files in folder %s!\n", (int)files.size(), source.c_str());
    } else if (getFile(source, files) >= 0) {
        printf("found %d image files in file %s!\n", (int)files.size(), source.c_str());
    } else {
        printf("could not load file list! wrong path / file?\n");
    }

    // get HZ
    std::string hertz;
    double hz;
    ros::param::get("~hz", hz);
    fprintf(stderr, "hz = %f\n", hz);

    if (!ros::param::get("~hz", hz))
        hz = 0;
    ros::param::del("~hz");

    cv::Mat image = cv::Mat(h, w, CV_8U);
    int runningIDX = 0;
    float fakeTimeStamp = 0;

    ros::Rate r(hz);

    std::string outfile;
    if (!ros::param::get("~rpg", outfile)) {
        outfile = "/home/serov/lsd_slam_estimate.txt";
    }

    // get timestamps of conti measurements
    std::string timestampFilename = source.substr(0, source.find_last_of('/')+1) + "times.txt";
    fprintf(stderr, "timestampsfile = %s\n", timestampFilename.c_str());
    std::ifstream  timestampFile;
    timestampFile.open(timestampFilename);
    if (! timestampFile.is_open()) {
        fprintf(stderr, "Couldnt open timestamps file: %s\n", timestampFilename.c_str());
        return -1;
    }
    std::vector<int> timestampIDs;
    std::vector<double> timestamps;
    int ID;
    double ts;
    while(timestampFile >> ID >> ts) {
        timestampIDs.push_back(ID);
        timestamps.push_back(ts);
    }
    fprintf(stderr, "Found %d timestamps\n", (int)timestamps.size());

    std::string outfileDir = outfile.substr(0, outfile.find_last_of('/'));
    std::string mkdirCmd = "mkdir " + outfileDir;
    std::system(mkdirCmd.c_str());
    fprintf(stderr, "outfile directory %s\n", outfile.c_str());

    std::ofstream rpgFile;
    rpgFile.open(outfile);
    if (!rpgFile.is_open()) {
        fprintf(stderr, "couldnt open %s to write output\n", outfile.c_str());
        return -1;
    }
    rpgFile << "# Timestamp tx ty tz qx qy qz qw" << std::endl;
    rpgFile.close();

    for (unsigned int i = 0; i < files.size(); i++) {
        cv::Mat imageDist = cv::imread(files[i], CV_LOAD_IMAGE_GRAYSCALE);

        if (imageDist.rows != h_inp || imageDist.cols != w_inp) {
            if (imageDist.rows * imageDist.cols == 0)
                printf("failed to load image %s! skipping.\n", files[i].c_str());
            else
                printf("image %s has wrong dimensions - expecting %d x %d, found %d x %d. Skipping.\n", files[i].c_str(), w, h,
                       imageDist.cols, imageDist.rows);
            continue;
        }
        assert(imageDist.type() == CV_8U);

        undistorter->undistort(imageDist, image);
        assert(image.type() == CV_8U);

        if (i == 0) {
            fprintf(stderr, "output width %d, output height %d", image.size().width, image.size().height);
        }

        double current_timestamp = timestamps[i];
        if (runningIDX == 0)
            system->randomInit(image.data, current_timestamp, runningIDX);
        else
            system->trackFrame(image.data, runningIDX, hz == 0, current_timestamp);
        runningIDX++;
        fakeTimeStamp += 0.0333;

        SE3 pose = system->getCurrentPoseEstimate();
        // fprintf(stderr, "ts %f, position %f, %f, %f, quat %f, %f, %f, %f\n", fakeTimeStamp, pose.translation().x(),
        // pose.translation().y(),
        //         pose.translation().z(), pose.unit_quaternion().x(), pose.unit_quaternion().y(), pose.unit_quaternion().z(),
        //         pose.unit_quaternion().w());
        rpgFile.open(outfile, std::ios_base::app);
        rpgFile << std::fixed << std::setprecision(6) << current_timestamp << std::setprecision(16) << " " << pose.translation().x() << " " << pose.translation().y() << " " << pose.translation().z() << " "
                << pose.unit_quaternion().x() << " " << pose.unit_quaternion().y() << " " << pose.unit_quaternion().z() << " "
                << pose.unit_quaternion().w() << std::endl;
        rpgFile.close();

        if (hz != 0)
            r.sleep();

        if (fullResetRequested) {

            printf("FULL RESET!\n");
            delete system;

            system = new SlamSystem(w, h, K, doSlam);
            system->setVisualization(outputWrapper);

            fullResetRequested = false;
            runningIDX = 0;
        }

        ros::spinOnce();

        if (!ros::ok())
            break;
    }

    system->finalize();

    delete system;
    delete undistorter;
    delete outputWrapper;
    return 0;
}
