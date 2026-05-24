CXX = g++
CXXFLAGS := $(shell pkg-config --cflags gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-app-1.0 json-glib-1.0 libsoup-3.0 opencv4) -I/usr/local/include/onnxruntime -Wall -g -std=c++17 -DGST_USE_UNSTABLE_API
LIBS := $(shell pkg-config --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gstreamer-app-1.0 json-glib-1.0 libsoup-3.0 opencv4) -L/usr/local/lib -lonnxruntime -pthread

OBJS = webrtc_send_ws.o face_detect.o
TARGET = webrtc_send_ws

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CXX) -o $@ $^ $(LIBS)

webrtc_send_ws.o: webrtc_send_ws.cpp face_detect.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

face_detect.o: face_detect.cpp face_detect.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) *.o
