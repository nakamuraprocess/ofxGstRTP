#ifndef ofxGstRTP_H_
#define ofxGstRTP_H_

#include "ofGstUtils.h"
#include <gst/app/gstappsink.h>
#include "ofxGstVideoDoubleBuffer.h"

//#include "ofParameter.h"
//#include "ofParameterGroup.h"


/// Client part implementing the RTP protocol. Allows to receive audio,
/// video, depth and metadata through osc from a remote peer. All the channels
/// will be synchronized and the communication can be started specifying the
/// ip and port of the remote side or using ofxNice streams.
class ofxGstRTP : public ofGstAppSink {
public:
	ofxGstRTP();
	virtual ~ofxGstRTP();

	/// use this version of setup when working with direct connection
	/// to an specific IP and port, usually in LANs when there's no need
	/// for NAT transversal.
	/// the latency parameter specifies a latency in milliseconds so the client
	/// buffers the received data to make the conection more reliable. The latency
	/// can be adjusted afterwards, but this will be the maximum latency the client can
	/// set later
	void setup(string srcIP, int latency);

	/// add an audio channel receiving in a specific port, has to be the same port
	/// specified in the server. Ports for the different channels will really occupy
	/// the next 5 ports so if we specify 3000, 3000-3005 will be used and shouldn't
	/// be specified for other channel
	void addAudioChannel(int port);
	/// add an video channel receiving in a specific port. Ports for the different channels will really occupy
	/// the next 5 ports so if we specify 3000, 3000-3005 will be used and shouldn't
	/// be specified for other channel
	void addVideoChannel(int port, string codec);

	void isRecordVideo(bool bRecord, string filePath);

	/// close the current connection
	void close();

	/// starts the gstreamer pipeline
	void play();

	/// update the pipeline and receive any available buffers
	void update();

	/// returns true if there's a new video frame after calling update
	bool isFrameNewVideo();

	/// get the pixels for the last frame received for the video channel
	ofPixels & getPixelsVideo();

	/// this paramter adjusts the latency on the client side to a maximum of the
	/// value set in setup
	ofParameter<int> latency;

	/// sets if the client should drop frames or accumulate them in a buffer,
	/// if the application doens't read fast enough this can cause a grow in memory
	/// but dropping frames can have the effect of dropping a key frame leading to
	/// glitches in the video and depth streams
	ofParameter<bool> drop;

	ofEvent<void> disconnectedEvent;

	static string LOG_NAME;

	bool getClientSenderTimeout();


private:
	void requestKeyFrame();
	void latencyChanged(int & latency);
	void dropChanged(bool & drop);

	struct NetworkElementsProperties{
		GstElement ** source;
		GstElement ** rtpcsource;
		GstElement ** rtpcsink;
		string srcIP;
		string capsstr;
		string capsfiltername;
		int port;
		int rtpcsrcport;
		int rtpcsinkport;
		int sessionNumber;
		string sourceName, rtpcSourceName, rtpcSinkName;
	};

	void createNetworkElements(NetworkElementsProperties properties, void *);

	void createAudioChannel(string rtpCaps);
	void createVideoChannel(string rtpCaps, int codec);

	// calbacks from gstUtils
	bool on_message(GstMessage * msg);
	void on_stream_prepared();
	void on_eos();

	// signal handlers for rtpc
	static void on_ssrc_active_handler(GstBin * rtpbin, guint session, guint ssrc, ofxGstRTP * rtpClient);
	static void on_new_ssrc_handler(GstBin *rtpbin, guint session, guint ssrc, ofxGstRTP * rtpClient);
	static void on_bye_ssrc_handler(GstBin *rtpbin, guint session, guint ssrc, ofxGstRTP * rtpClient);
	static void on_pad_added(GstBin *rtpbin, GstPad *pad, ofxGstRTP * rtpClient);
	static void on_sender_timeout_handler(GstBin* rtpbin, guint session, guint ssrc, ofxGstRTP* rtpClient);

	// video callbacks
	static void on_eos_from_video(GstAppSink * elt, void * rtpClient);
	static GstFlowReturn on_new_preroll_from_video(GstAppSink * elt, void * rtpClient);
	static GstFlowReturn on_new_buffer_from_video(GstAppSink * elt, void * rtpClient);

	// video instance callbacks
	void on_eos_from_video(GstAppSink * elt){};
	GstFlowReturn on_new_preroll_from_video(GstAppSink * elt){return GST_FLOW_OK;}
	GstFlowReturn on_new_buffer_from_video(GstAppSink * elt);

	void linkVideoPad(GstPad * pad);
	void linkAudioPad(GstPad * pad);

	ofGstUtils gst;
	ofGstUtils gstAudioOut;
	GstMapInfo mapinfo;

	GstElement * pipeline;
	GstElement * pipelineAudioOut;
	GstElement * rtpbin;

	GstElement * rtpVideodepay;
	GstElement * opusdepay;
	GstElement * gstdepay;

	GstElement* tee;
	GstElement* parse;
	GstElement* queue1;
	GstElement* queue2;
	GstElement* vmux;
	GstElement* fileSink;

	GstAppSink * videoSink;

	GstElement * vudpsrc;
	GstElement * audpsrc;
	GstElement * vudpsrcrtcp;
	GstElement * audpsrcrtcp;

	GstElement * audioechosrc;
	GstElement * audioechosink;

	ofxGstVideoDoubleBuffer<unsigned char> doubleBufferVideo;

	string src;

	int videoSessionNumber;
	int audioSessionNumber;
	int lastSessionNumber;

	guint videoSSRC;
	guint audioSSRC;

	bool videoReady;
	bool audioReady;

	bool recordVideo = false;
	string recordVideoPath = "";

	bool bClientSenderTimeout;
};

#endif /* ofxGstRTPCLIENT_H_ */
