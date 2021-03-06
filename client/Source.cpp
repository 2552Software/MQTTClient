/*
* Copyright (c) 2014 Cesanta Software Limited
* All rights reserved
* This software is dual-licensed: you can redistribute it and/or modify
* it under the terms of the GNU General Public License version 2 as
* published by the Free Software Foundation. For the terms of this
* license, see <http://www.gnu.org/licenses/>.
*
* You are free to use this software under the terms of the GNU General
* Public License, but WITHOUT ANY WARRANTY; without even the implied
* warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
* See the GNU General Public License for more details.
*
* Alternatively, you can license this software under a commercial
* license, as set out in <https://www.cesanta.com/license>.
*/

// find libs here C:\OpenCV\opencv-3.3.1\build\install\x64\vc15\lib

#include "mongoose.h"
#include <iostream>
#include <fstream>
#include <stdio.h>
#include <iostream>
#include <vector>
#include "opencv2/imgcodecs.hpp"
#include "opencv2/highgui.hpp"
#include "opencv2/stitching.hpp"
#include "opencv2/core/core.hpp"
#include "opencv2/imgproc/imgproc.hpp"
#include "opencv2/video/background_segm.hpp"
#include "opencv2/bgsegm.hpp"
#include "opencv2/videoio.hpp"
#include "json.hpp"

// for convenience
using json = nlohmann::json;

/*
int test(){
	cv::Mat back;
	cv::Mat fore;
	pMOG2 = cv::createBackgroundSubtractorMOG2(); //MOG2 approach
	cv::BackgroundSubtractorMOG2 bg(5, 3, true);
	cv::namedWindow("Background");
	std::vector<std::vector<cv::Point> > contours;

	bg.operator ()(frame, fore);
	bg.getBackgroundImage(back);
	cv::erode(fore, fore, cv::Mat());
	cv::dilate(fore, fore, cv::Mat());
	cv::findContours(fore, contours, CV_RETR_EXTERNAL, CV_CHAIN_APPROX_NONE);
	cv::drawContours(frame, contours, -1, cv::Scalar(0, 0, 255), 2);
	return frame;
}
*/

// add some json, image stuff and good to go

static const char *s_address = "127.0.0.1:1883";// "192.168.88.100:1883";
static const char *s_user_name = NULL;
static const char *s_password = NULL;
static struct mg_mqtt_topic_expression s_topic_expr = { NULL, 0 };
bool try_use_gpu = false;
cv::Stitcher::Mode mode = cv::Stitcher::PANORAMA;
std::vector<cv::Mat> imgs;
std::string result_name = "result.jpg";
cv::Mat frame; //current frame
cv::Mat fgMaskMOG2; //fg mask fg mask generated by MOG2 method
cv::Ptr<cv::BackgroundSubtractor> pMOG2; //MOG2 Background subtractor
char keyboard; //input from keyboard
char datatopic[128];//bugbug rm magic 
static const char *s_topic = "/stuff";
std::vector<uchar> jpgbytes; // from your db
cv::VideoWriter *pVideoWriter = nullptr;
int count = 0;

// get raw from cam? what is the size difference? then let cv do the work?
void tofile(const std::string& name, cv::InputArray &img) {
	std::vector<int> compression_params; //vector that stores the compression parameters of the image

	compression_params.push_back(CV_IMWRITE_JPEG_QUALITY); //specify the compression technique
	compression_params.push_back(98); //specify the compression quality
	/*
	The image format is chosen depending on the file name extension. 
	Only images with 8 bit or 16 bit unsigned  single channel or 
	3 channel ( CV_8UC1, CV_8UC3, CV_8SC1, CV_8SC3, CV_16UC1, CV_16UC3) 
	with 'BGR' channel order, can be saved. If the depth or channel order 
	of the image is different, use 'Mat::convertTo()' or 'cvtColor' functions to 
	convert the image to supporting format before using imwrite function.

	params - This is a int vector to which you have to insert some int parameters specifying the format of the image
	JPEG format - You have to puch_back CV_IMWRITE_JPEG_QUALITY first and then a number between 0 and 100 (higher is the better). 
	If you want the best quality output, use 100. I have used 98 in the above sample program. 
	But higher the value, it will take longer time to write the image
	PNG format - You have to puch_back CV_IMWRITE_PNG_COMPRESSION first and then a number between 0 and 9 (higher is the better compression, but slower).
	*/
	bool bSuccess = cv::imwrite(name, img, compression_params); //write the image to file
}

void tovideo() {
	double dWidth = 100;//get real value from input cap.get(CV_CAP_PROP_FRAME_WIDTH); //get the width of frames of the video
	double dHeight = 100;// cap.get(CV_CAP_PROP_FRAME_HEIGHT); //get the height of frames of the video

	std::cout << "Frame Size = " << dWidth << "x" << dHeight << std::endl;

	cv::Size frameSize(static_cast<int>(dWidth), static_cast<int>(dHeight));
	cv::VideoWriter oVideoWriter("MyVideo.avi", CV_FOURCC('P', 'I', 'M', '1'), 20, frameSize, true); //initialize the VideoWriter object 

	if (!oVideoWriter.isOpened()) //if not initialize the VideoWriter successfully, exit the program
	{
		std::cout << "ERROR: Failed to write the video" << std::endl;
		return;
	}

	while (1)
	{
		cv::Mat frame;
		bool bSuccess = true;//cap.read(frame); // read a new frame from video
		if (!bSuccess) //if not success, break loop
		{
			std::cout << "ERROR: Cannot read a frame from video file" << std::endl;
			break;
		}

		oVideoWriter.write(frame); //writer the frame into the file
		imshow("myVideo", frame); //show the frame in "MyVideo" window
		if (cv::waitKey(10) == 27) //wait for 'esc' key press for 30ms. If 'esc' key is pressed, break loop
		{
			std::cout << "esc key is pressed by user" << std::endl;
			break;
		}
	}
}
static void ev_handler(struct mg_connection *nc, int ev, void *p) {
	struct mg_mqtt_message *msg = (struct mg_mqtt_message *) p;
	(void)nc;

	if (ev != MG_EV_POLL) printf("USER HANDLER GOT EVENT %d\n", ev);

	switch (ev) {
	case MG_EV_CONNECT: {
		struct mg_send_mqtt_handshake_opts opts;
		memset(&opts, 0, sizeof(opts));
		opts.user_name = s_user_name;
		opts.password = s_password;

		mg_set_protocol_mqtt(nc);
		mg_send_mqtt_handshake_opt(nc, "dummy", opts);
		break;
	}
	case MG_EV_MQTT_CONNACK:
		if (msg->connack_ret_code != MG_EV_MQTT_CONNACK_ACCEPTED) {
			printf("Got mqtt connection error: %d\n", msg->connack_ret_code);
			exit(1);
		}
		// setup upon connections
		//s_topic_expr.topic = s_topic;
		//printf("Subscribing to '%s'\n", s_topic);
		//mg_mqtt_subscribe(nc, &s_topic_expr, 1, 42);
		{
			struct mg_mqtt_topic_expression topicExpression;
			topicExpression.qos = 0;
			topicExpression.topic = "error";
			mg_mqtt_subscribe(nc, &topicExpression, 1, 13);
			topicExpression.topic = "trace";
			mg_mqtt_subscribe(nc, &topicExpression, 1, 14);
			topicExpression.topic = "dataready"; // json holds machine info
			mg_mqtt_subscribe(nc, &topicExpression, 1, 15);
			topicExpression.topic = "datafinal";
			mg_mqtt_subscribe(nc, &topicExpression, 1, 16);
			topicExpression.topic = "datacam1"; // every device needs this so no json etc need be sent
			mg_mqtt_subscribe(nc, &topicExpression, 1, 17); // can I subscribe only here?
			//mg_mqtt_publish(nc, "/test", 65, MG_MQTT_QOS(0), "hi", 2);
		}
		break;
	case MG_EV_MQTT_PUBACK:
		printf("Message publishing acknowledged (msg_id: %d)\n", msg->message_id);
		break;
	case MG_EV_MQTT_SUBACK:
		printf("Subscription acknowledged (msg_id: %d)\n", msg->message_id);
		//printf("Subscription acknowledged, forwarding to '/test'\n");
		break;
	case MG_EV_MQTT_PUBLISH: {
#if 0
		char hex[1024] = { 0 };
		mg_hexdump(nc->recv_mbuf.buf, msg->payload.len, hex, sizeof(hex));
		printf("Got incoming message %.*s:\n%s", (int)msg->topic.len, msg->topic.p, hex);
#else
		//printf("Got incoming message %.*s: %.*s\n", (int)msg->topic.len,
			//msg->topic.p, (int)msg->payload.len, msg->payload.p);
#endif
		/*
		void mg_mqtt_unsubscribe(struct mg_connection *nc, char **topics,
                         size_t topics_len, uint16_t message_id);
		*/

		// need to support > 1 camera soon

		if (mg_mqtt_vmatch_topic_expression("trace", msg->topic)) {
			// echo json and all at least for now
			printf("Trace message %.*s: %.*s\n", (int)msg->topic.len, msg->topic.p, (int)msg->payload.len, msg->payload.p);
		}
		else if (mg_mqtt_vmatch_topic_expression("error", msg->topic)) {
			printf("ERROR message %.*s: %.*s\n", (int)msg->topic.len, msg->topic.p, (int)msg->payload.len, msg->payload.p);
		}
		else if (mg_mqtt_vmatch_topic_expression("dataready", msg->topic)) {
			std::string s(msg->payload.p, msg->payload.len); // not null terminated
			json j = json::parse(s);
			std::string path = j["path"];
			std::string name = j["name"];
			printf("data ready %s %s\n", name.c_str(), path.c_str());
			jpgbytes.clear(); // early draft of ideas here...
			// create the file but this will
			//need OpenCV wstream = fs.createWriteStream(cam1);
		}
		else if (mg_mqtt_vmatch_topic_expression("datafinal", msg->topic)) {
			std::string s(msg->payload.p, msg->payload.len); // not null terminated
			json j = json::parse(s);
			std::string path = j["path"];
			printf("data final (EOF) %s\n", path.c_str());
			// incase we go to raw cv::Mat mat(height,width,CV_8UC3,string.data());   
			if (jpgbytes.size() > 0) {
				cv::Mat data_mat(jpgbytes, true);
				if (!data_mat.empty()) {
#define MAX_COUNT 250   
#define DELAY_T 3
#define PI 3.1415   

					IplImage* image = 0;

					//T, T-1 image   
					IplImage* current_Img = 0;
					IplImage* Old_Img = 0;

					//Optical Image   
					IplImage * imgA = 0;
					IplImage * imgB = 0;

					IplImage * eig_image = 0;
					IplImage * tmp_image = 0;
					int corner_count = MAX_COUNT;
					CvPoint2D32f* cornersA = new CvPoint2D32f[MAX_COUNT];
					CvPoint2D32f * cornersB = new CvPoint2D32f[MAX_COUNT];

					CvSize img_sz;
					int win_size = 20;

					IplImage* pyrA = 0;
					IplImage* pyrB = 0;

					char features_found[MAX_COUNT];
					float feature_errors[MAX_COUNT];
					//////////////////////////////////////////////////////////////////////////   


					//////////////////////////////////////////////////////////////////////////   
					//Variables for time different video   
					int one_zero = 0;
					//int t_delay=0;   

					double gH[9] = { 1,0,0, 0,1,0, 0,0,1 };
					CvMat gmxH = cvMat(3, 3, CV_64F, gH);

					//RGB to Gray for Optical Flow   
//					cv::cvCvtColor(current_Img, imgA, CV_BGR2GRAY);
	//				cvCvtColor(Old_Img, imgB, CV_BGR2GRAY);

					//extract features next step, remove low change items
					cvGoodFeaturesToTrack(imgA, eig_image, tmp_image, cornersA, &corner_count, 0.01, 5.0, 0, 3, 0, 0.04);
					cvFindCornerSubPix(imgA, cornersA, corner_count, cvSize(win_size, win_size), cvSize(-1, -1), cvTermCriteria(CV_TERMCRIT_ITER | CV_TERMCRIT_EPS, 20, 0.03));
					
					cv::Mat image2(cv::imdecode(data_mat, 1)); //put 0 if you want greyscale, 1 color
					size_t sizeInBytes = image2.total() * image2.elemSize();
					std::vector<uchar> buff;//buffer for coding
					std::vector<int> param(2);
					param[0] = cv::IMWRITE_JPEG_QUALITY;
					param[1] = 30;//default(95) 0-100
					//cv::imencode(".jpg", image2, buff, param);
					imshow("Frame", image2);
					std::string name = "bwz" + path;
					cv::imwrite(name, image2, param);

					cv::Mat edges;
					cv::cvtColor(image2, edges, cv::COLOR_BGR2GRAY);
					GaussianBlur(edges, edges, cv::Size(7, 7), 1.5, 1.5);
					Canny(edges, edges, 0, 30, 3);
					sizeInBytes = edges.total() * edges.elemSize();
					imshow("edges", edges);
					cv::waitKey(10);
					/*
					//cv::imwrite("test.png", edges);
					printf("pVideoWriter->isOpened\n");
					if (++count == 1) {
						//  CV_FOURCC('P', 'I', 'M', '1') or ? CV_FOURCC('M', 'J', 'P', 'G')
						pVideoWriter = new cv::VideoWriter("MyVideoabc.avi", CV_FOURCC('P', 'I', 'M', '1'), 20, cv::Size(640, 480), true); //initialize the VideoWriter object 
						if (pVideoWriter && pVideoWriter->isOpened()) {
							printf("pVideoWriter->isOpened\n");
						}
					}
					else if (count == 100) {
						pVideoWriter->release(); // should be about 1 minute
						delete pVideoWriter;
						//  CV_FOURCC('P', 'I', 'M', '1') or ? CV_FOURCC('M', 'J', 'P', 'G')
						pVideoWriter = new cv::VideoWriter("MyVideo21.avi", CV_FOURCC('P', 'I', 'M', '1'), 20, cv::Size(640, 480), true); //initialize the VideoWriter object 
					}
					else if (count == 200) {
						pVideoWriter->release();
						delete pVideoWriter;
						pVideoWriter = new cv::VideoWriter("MyVideo32.avi", CV_FOURCC('P', 'I', 'M', '1'), 20, cv::Size(640, 480), true); //initialize the VideoWriter object 
					}
					else if (count == 300) {
						pVideoWriter->release();
						delete pVideoWriter;
						pVideoWriter = new cv::VideoWriter("MyVideo43.avi", CV_FOURCC('P', 'I', 'M', '1'), 20, cv::Size(640, 480), true); //initialize the VideoWriter object 
					}
					else if (count == 500) {
						pVideoWriter->release();
						delete pVideoWriter;
						pVideoWriter = new cv::VideoWriter("MyVideo54.avi", CV_FOURCC('P', 'I', 'M', '1'), 20, cv::Size(640, 480), true); //initialize the VideoWriter object 
					}
					if (pVideoWriter) {
						pVideoWriter->write(image2);
					}
					cv::waitKey(100);
					*/
				}
			}
			//wstream.end();
		}
		else if (mg_mqtt_vmatch_topic_expression("datacam1", msg->topic)) {
			printf("raw data len %zd\n", msg->payload.len);
			for (int i = 0; i < msg->payload.len; ++i) {
				jpgbytes.push_back((uchar)msg->payload.p[i]);
			}
			//mg_hexdump(nc->recv_mbuf.buf, msg->payload.len, hex, sizeof(hex));
			//wstream.write(message);
		}
		return;
		std::string path = "C:\\projects\\engine\\";
		for (int i = 0; i < msg->payload.len; ++i) {
			path += msg->payload.p[i];
		}
		size_t len = path.length();
		cv::Mat background;
		cv::Mat foreground;
		cv::Mat frame = cv::imread(path);
		std::vector<std::vector<cv::Point> > contours;
		if (!frame.empty()) {
			pMOG2->apply(frame, fgMaskMOG2);
			pMOG2->getBackgroundImage(background);
			imshow("back", background);
			imshow("Frame", frame);
			imshow("mog", fgMaskMOG2);
			//cv::imshow("Display window", img);
			cv::waitKey(25);
		}
		printf("Forwarding to /test\n");
		mg_mqtt_publish(nc, "/test", 65, MG_MQTT_QOS(0), msg->payload.p,
			msg->payload.len);
		break;
	}
	case MG_EV_CLOSE:
		printf("Connection closed\n");
		exit(1);
	}
}

int main(int argc, char **argv) {
	cv::namedWindow("back", cv::WINDOW_AUTOSIZE);
	cv::namedWindow("Frame", cv::WINDOW_AUTOSIZE);
	cv::namedWindow("mog", cv::WINDOW_AUTOSIZE);
	cv::namedWindow("myVideo", cv::WINDOW_AUTOSIZE);
	//create Background Subtractor objects
	pMOG2 = cv::createBackgroundSubtractorMOG2(); //MOG2 approach

	struct mg_mgr mgr;

	// bw? image = cv::imread(img1, CV_LOAD_IMAGE_COLOR);   // Read the file

	/* stretch later
	cv::Mat pano;
	cv::Ptr<cv::Stitcher> stitcher = cv::Stitcher::create(mode, try_use_gpu);
	cv::Stitcher::Status status = stitcher->stitch(imgs, pano);
	if (status != cv::Stitcher::OK)	{
		std::cout << "Can't stitch images, error code = " << int(status) << std::endl;
		//return -1;
	}
	imwrite(result_name, pano);
	*/
	mg_mgr_init(&mgr, NULL);

	if (mg_connect(&mgr, s_address, ev_handler) == NULL) {
		fprintf(stderr, "mg_connect(%s) failed\n", s_address);
		exit(EXIT_FAILURE);
	}

	for (;;) {
		mg_mgr_poll(&mgr, 1000);
	}
}
