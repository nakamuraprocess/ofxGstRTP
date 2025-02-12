#include "ofMain.h"

#include "ofxGstRTP.h"
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include <glib-object.h>
#include <glib.h>
#include <list>

#define RTPBIN_MAX_LATENCY 2000
string ofxGstRTP::LOG_NAME = "ofxGstRTP";


//  receives H264 encoded RTP video on port 5000, RTCP is received on  port 5001.
//  the receiver RTCP reports are sent to port 5005
//
//  receives OPUS encoded RTP audio on port 5002, RTCP is received on  port 5003.
//  the receiver RTCP reports are sent to port 5007
//
//             .-------.      .----------.     .---------.   .-------.   .-----------.
//  RTP        |udpsrc |      | rtpbin   |     |h264depay|   |h264dec|   |appsink    |
//  port=5000  |      src->recv_rtp recv_rtp->sink     src->sink   src->sink         |
//             '-------'      |          |     '---------'   '-------'   '-----------'
//                            |          |
//                            |          |     .-------.
//                            |          |     |udpsink|  RTCP
//                            |    send_rtcp->sink     | port=5005
//             .-------.      |          |     '-------' sync=false
//  RTCP       |udpsrc |      |          |               async=false
//  port=5001  |     src->recv_rtcp      |
//             '-------'      |          |
//                            |          |
//             .-------.      |          |     .---------.   .-------.   .-------------.
//  RTP        |udpsrc |      | rtpbin   |     |opusdepay|   |opusdec|   |autoaudiosink|
//  port=5002  |      src->recv_rtp recv_rtp->sink     src->sink   src->sink           |
//             '-------'      |          |     '---------'   '-------'   '-------------'
//                            |          |
//                            |          |     .-------.
//                            |          |     |udpsink|  RTCP
//                            |    send_rtcp->sink     | port=5007
//             .-------.      |          |     '-------' sync=false
//  RTCP       |udpsrc |      |          |               async=false
//  port=5003  |     src->recv_rtcp      |
//             '-------'      '----------'

ofxGstRTP::ofxGstRTP()
	:pipeline(0)
	, rtpbin(0)
	, rtpVideodepay(0)
	, opusdepay(0)
	, gstdepay(0)
	, tee(0)
	, parse(0)
	, vmux(0)
	, queue1(0)
	, queue2(0)
	, fileSink(0)
	, videoSink(0)
	, vudpsrc(0)
	, audpsrc(0)
	, vudpsrcrtcp(0)
	, audpsrcrtcp(0)
	, videoSessionNumber(-1)
	, audioSessionNumber(-1)
	, videoSSRC(0)
	, audioSSRC(0)
	, videoReady(false)
	, audioReady(false)
	, lastSessionNumber(0)
	, audioechosrc(NULL)
{
	GstMapInfo initMapinfo = { 0, };
	mapinfo = initMapinfo;

	latency.set("latency", 0, 0, RTPBIN_MAX_LATENCY);
	latency.addListener(this, &ofxGstRTP::latencyChanged);
	drop.set("drop", false);
	drop.addListener(this, &ofxGstRTP::dropChanged);

	bClientSenderTimeout = false;
}

ofxGstRTP::~ofxGstRTP() {
	close();
}


static string get_object_structure_property(GObject* object, const string& property) {
	GstStructure* structure;
	gchar* str;

	if (object == NULL) return "";

	/* get the source stats */
	g_object_get(object, property.c_str(), &structure, NULL);

	/* simply dump the stats structure */
	str = gst_structure_to_string(structure);

	gst_structure_free(structure);
	string ret = str;
	g_free(str);
	return ret;
}


void ofxGstRTP::on_ssrc_active_handler(GstBin* rtpbin, guint session, guint ssrc, ofxGstRTP* rtpClient) {
	GObject* internalSession;
	g_signal_emit_by_name(rtpbin, "get-internal-session", session, &internalSession, NULL);
	ofLogVerbose(LOG_NAME) << "ssrc active " << G_OBJECT_CLASS_NAME(G_OBJECT_GET_CLASS(internalSession)) << " for session " << session;

	GObject* internalSource;
	g_object_get(internalSession, "internal-source", &internalSource, NULL);
	//ofLogVerbose(LOG_NAME) << get_object_structure_property(internalSource,"stats");

	GObject* remoteSource;
	g_signal_emit_by_name(internalSession, "get-source-by-ssrc", ssrc, &remoteSource, NULL);
}


void ofxGstRTP::on_new_ssrc_handler(GstBin* rtpbin, guint session, guint ssrc, ofxGstRTP* rtpClient) {
	ofLogVerbose(LOG_NAME) << "new ssrc " << ssrc << " for session " << session;
	GObject* internalSession;
	g_signal_emit_by_name(rtpbin, "get-internal-session", session, &internalSession, NULL);

	GObject* remoteSource;
	g_signal_emit_by_name(internalSession, "get-source-by-ssrc", ssrc, &remoteSource, NULL);


	GObject* internalSource;
	g_object_get(internalSession, "internal-source", &internalSource, NULL);
	ofLogVerbose(LOG_NAME) << get_object_structure_property(internalSource, "stats");

	GstStructure* stats;
	gchar* remoteAddress = 0;
	g_object_get(remoteSource, "stats", &stats, NULL);
	gst_structure_get(stats, "rtcp-from", G_TYPE_STRING, &remoteAddress, NULL);

	if (remoteAddress) {
		ofLogVerbose(LOG_NAME) << "new client connected from " << remoteAddress;
		g_free(remoteAddress);
	}
	else {
		ofLogVerbose(LOG_NAME) << "couldn't get remote";
	}
	ofLogVerbose(LOG_NAME) << get_object_structure_property(remoteSource, "stats");

}


void ofxGstRTP::on_pad_added(GstBin* rtpbin, GstPad* pad, ofxGstRTP* rtpClient) {
	// when a pad is added to the rtbbin, connect the video and depth elements to the correct pads
	// FIXME: there must be a better way to detect the correct pad than it's partial name

	string padName = gst_object_get_name(GST_OBJECT(pad));


	ofLogVerbose(LOG_NAME) << "new pad " << gst_object_get_name(GST_OBJECT(pad));

	if (ofIsStringInString(padName, "recv_rtp_src_" + ofToString(rtpClient->videoSessionNumber))) {
		ofLogVerbose(LOG_NAME) << "video pad created";
		rtpClient->linkVideoPad(pad);

	}
	else if (ofIsStringInString(padName, "recv_rtp_src_" + ofToString(rtpClient->audioSessionNumber))) {
		ofLogVerbose(LOG_NAME) << "audio pad created";
		rtpClient->linkAudioPad(pad);

	}
}

void ofxGstRTP::linkAudioPad(GstPad* pad) {
	GstPad* sinkPad = gst_element_get_static_pad(opusdepay, "sink");
	if (sinkPad) {
		if (gst_pad_link(pad, sinkPad) != GST_PAD_LINK_OK) {
			ofLogError(LOG_NAME) << "couldn't link rtp source pad to audio depay";
		}
		else {
			ofLogVerbose(LOG_NAME) << "audio pipeline complete!";
			audioReady = true;
		}
	}
	else {
		ofLogError(LOG_NAME) << "couldn't get sink pad for opus depay";
	}
}

void ofxGstRTP::linkVideoPad(GstPad* pad) {

	GstPad* sinkPad = gst_element_get_static_pad(rtpVideodepay, "sink");
	if (sinkPad) {
		if (gst_pad_link(pad, sinkPad) != GST_PAD_LINK_OK) {
			ofLogError(LOG_NAME) << "couldn't link rtp source pad to video depay";
		}
		else {
			ofLogVerbose(LOG_NAME) << "video pipeline complete!";
			videoReady = true;

			int currentLatency = latency;
			latencyChanged(currentLatency);
		}
	}
	else {
		ofLogError(LOG_NAME) << "couldn't get sink pad for video depay";
	}
}


void ofxGstRTP::on_bye_ssrc_handler(GstBin* rtpbin, guint session, guint ssrc, ofxGstRTP* rtpClient) {
	ofLogVerbose(LOG_NAME) << "client disconnected";
}

void ofxGstRTP::on_sender_timeout_handler(GstBin* rtpbin, guint session, guint ssrc, ofxGstRTP* rtpClient) {
	ofLogVerbose(LOG_NAME) << "client timeout";
	rtpClient->bClientSenderTimeout = true;
}


void ofxGstRTP::createNetworkElements(NetworkElementsProperties properties, void*) {

	// create network elements for the stream and add them to the pipeline
	GstCaps* caps = gst_caps_from_string(properties.capsstr.c_str());

	*properties.source = gst_element_factory_make("udpsrc", properties.sourceName.c_str());
	g_object_set(G_OBJECT(*properties.source), "port", properties.port, "caps", caps, NULL);
	gst_caps_unref(caps);

	gst_bin_add(GST_BIN(pipeline), *properties.source);

	GstPad* sinkpad = gst_element_get_request_pad(rtpbin, ("recv_rtp_sink_" + ofToString(properties.sessionNumber)).c_str());
	GstPad* srcpad = gst_element_get_static_pad(*properties.source, "src");
	if (!sinkpad) {
		ofLogError(LOG_NAME) << "couldn't get rtpbin sink for session " << properties.sessionNumber;
	}
	if (gst_pad_link(srcpad, sinkpad) != GST_PAD_LINK_OK) {
		ofLogError(LOG_NAME) << "couldn't link src to rtpbin";
	}

	*properties.rtpcsource = gst_element_factory_make("udpsrc", properties.rtpcSourceName.c_str());
	g_object_set(G_OBJECT(*properties.rtpcsource), "port", properties.rtpcsrcport, NULL);


	gst_bin_add(GST_BIN(pipeline), *properties.rtpcsource);
	GstPad* rtcpsinkpad = gst_element_get_request_pad(rtpbin, ("recv_rtcp_sink_" + ofToString(properties.sessionNumber)).c_str());
	GstPad* rtcpsrcpad = gst_element_get_static_pad(*properties.rtpcsource, "src");
	if (gst_pad_link(rtcpsrcpad, rtcpsinkpad) != GST_PAD_LINK_OK) {
		ofLogError(LOG_NAME) << "couldn't link rtpc src to rtpbin";
	}

	// create rtcp sink
	*properties.rtpcsink = gst_element_factory_make("udpsink", properties.rtpcSinkName.c_str());
	g_object_set(G_OBJECT(*properties.rtpcsink), "port", properties.rtpcsinkport, "host", properties.srcIP.c_str(), "sync", 0, "force-ipv4", 1, "async", 0, NULL);

	gst_bin_add(GST_BIN(pipeline), *properties.rtpcsink);

	rtcpsrcpad = gst_element_get_request_pad(rtpbin, ("send_rtcp_src_" + ofToString(properties.sessionNumber)).c_str());
	rtcpsinkpad = gst_element_get_static_pad(*properties.rtpcsink, "sink");
	if (gst_pad_link(rtcpsrcpad, rtcpsinkpad) != GST_PAD_LINK_OK) {
		ofLogError(LOG_NAME) << "couldn't link rptbin src to rtpc sink";
	}
}

void ofxGstRTP::createVideoChannel(string rtpCaps, int codec) {
	videoSessionNumber = lastSessionNumber;
	lastSessionNumber++;

	// create and add video and depth elements and connect them to the correct pad.
	// if we don't do this after the pad has been created when the connection is detected,
	// gstreamer tries to link this elements by their capabilities and
	// since video and depth have the same it sometimes swap them and you end getting rgb on the depth sink
	// and viceversa

	// rgb pipeline to be connected to the corresponding recv_rtp_send pad:
	// rtph264depay ! avdec_h264 ! videoconvert ! appsink

	const char* depayName[2][2] = {
		{"rtph264depay", "rtph264depay_video"},
		{"rtph265depay", "rtph265depay_video"}
	};

	const char* avdecName[2][2] = {
		{"avdec_h264", "avdec_h264_video"},
		{"avdec_h265", "avdec_h265_video"}
	};

	rtpVideodepay = gst_element_factory_make(depayName[codec][0], depayName[codec][1]);
	GstElement* avdec = gst_element_factory_make(avdecName[codec][0], avdecName[codec][1]);
	GstElement* vconvert = gst_element_factory_make("videoconvert", "vconvert");
	videoSink = (GstAppSink*)gst_element_factory_make("appsink", "videosink");
	g_object_set(G_OBJECT(videoSink), "sync", FALSE, "async", FALSE, NULL);


	// set format for video appsink to rgb
	GstCaps* caps = NULL;
	caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", NULL);

	if (!caps) {
		ofLogError(LOG_NAME) << "couldn't get caps";
	}
	else {
		gst_app_sink_set_caps(videoSink, caps);
		gst_caps_unref(caps);
	}

	// set callbacks to receive rgb data
	GstAppSinkCallbacks gstCallbacks;
	gstCallbacks.eos = &ofxGstRTP::on_eos_from_video;
	gstCallbacks.new_preroll = &ofxGstRTP::on_new_preroll_from_video;
	gstCallbacks.new_sample = &ofxGstRTP::on_new_buffer_from_video;
	gst_app_sink_set_callbacks(GST_APP_SINK(videoSink), &gstCallbacks, this, NULL);
	gst_app_sink_set_emit_signals(GST_APP_SINK(videoSink), 0);


	// add elements to the pipeline and link them (but not yet to the rtpbin)
	if (recordVideo) {
		const char* parseName[2] = { "h264parse", "h265parse" };
		parse = gst_element_factory_make(parseName[codec], NULL);
		tee = gst_element_factory_make("tee", "tee");
		queue1 = gst_element_factory_make("queue", "queue1");
		queue2 = gst_element_factory_make("queue", "queue2");

		vmux = gst_element_factory_make("matroskamux", "matroskamux");
		fileSink = gst_element_factory_make("filesink", "filesink");

		gst_bin_add_many(GST_BIN(pipeline), rtpVideodepay, parse, tee, queue1, vmux, fileSink, queue2, avdec, vconvert, videoSink, NULL);
		if (!gst_element_link_many(rtpVideodepay, parse, tee, NULL)) {
			ofLogError(LOG_NAME) << "couldn't link tee elements";
		}
		if (!gst_element_link_many(tee, queue1, vmux, fileSink, NULL)) {
			ofLogError(LOG_NAME) << "couldn't link filesink elements";
		}
		if (!gst_element_link_many(tee, queue2, avdec, vconvert, videoSink, NULL)) {
			ofLogError(LOG_NAME) << "couldn't link videosink elements";
		}

		g_object_set(G_OBJECT(fileSink), "location", recordVideoPath.c_str(), NULL);


		/*GstPad* teepad1;
		GstPad* teepad2;
		GstPad* qpad1;
		GstPad* qpad2;

		GstPadTemplate* tee_src_pad_template;
		if (!(tee_src_pad_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(tee), "src_%u"))) {
			g_critical("Unable to get pad template!");
			return;
		}

		// get the queue pads and request some source pads from the tee
		qpad1 = gst_element_get_static_pad(queue1, NULL);
		qpad2 = gst_element_get_static_pad(queue2, NULL);
		teepad1 = gst_element_request_pad(tee, tee_src_pad_template, NULL, NULL);
		teepad2 = gst_element_request_pad(tee, tee_src_pad_template, NULL, NULL);

		if (!teepad1 || !teepad2 || !qpad1 || !qpad2) {
			g_error("Creation of one tee pad failed.");
			return;
		}


		gst_pad_link(teepad1, qpad1);
		gst_pad_link(teepad2, qpad2);

		gst_object_unref(GST_OBJECT(qpad1));
		gst_object_unref(GST_OBJECT(qpad2));
		gst_object_unref(GST_OBJECT(teepad1));
		gst_object_unref(GST_OBJECT(teepad2));
		*/
	}
	else {
		gst_bin_add_many(GST_BIN(pipeline), rtpVideodepay, avdec, vconvert, videoSink, NULL);
		if (!gst_element_link_many(rtpVideodepay, avdec, vconvert, videoSink, NULL)) {
			ofLogError(LOG_NAME) << "couldn't link video elements";
		}
	}
}

void ofxGstRTP::createAudioChannel(string rtpCaps) {
	audioSessionNumber = lastSessionNumber;
	lastSessionNumber++;

	// create and add audio elements and connect them to the correct pad.
	// audio pipeline to be connected to the corresponding recv_rtp_send pad:
	// Linux:
	// rtpopusdepay ! opusdec ! audioconvert ! audioresample ! pulsesink stream-properties=\"props,media.role=phone,filter.want=echo-cancel\"
	// everything else:
	// rtpopusdepay ! opusdec ! audioconvert ! audioresample ! autoaudiosink

	opusdepay = gst_element_factory_make("rtpopusdepay", "rtpopusdepay1");
	GstElement* opusdec = gst_element_factory_make("opusdec", "opusdec1");
	GstElement* audioconvert = gst_element_factory_make("audioconvert", "audioconvert1");
	GstElement* audioresample = gst_element_factory_make("audioresample", "audioresample1");


#ifdef TARGET_LINUX
	GstElement* audiosink = gst_element_factory_make("pulsesink", "pulsesink1");
	GstStructure* pulseProperties;
#if ENABLE_ECHO_CANCEL
	if (echoCancel) {
		pulseProperties = gst_structure_new("props", "media.role", G_TYPE_STRING, "phone", NULL);
	}
	else
#endif
		pulseProperties = gst_structure_new("props", "media.role", G_TYPE_STRING, "phone", "filter.want", G_TYPE_STRING, "echo-cancel", NULL);

	g_object_set(audiosink, "stream-properties", pulseProperties, NULL);
#else
	GstElement* audiosink = gst_element_factory_make("autoaudiosink", "autoaudiosink1");
#endif

	{
		// add elements to the pipeline and link them (but not yet to the rtpbin)
		gst_bin_add_many(GST_BIN(pipeline), opusdepay, opusdec, audioconvert, audioresample, audiosink, NULL);
		if (!gst_element_link_many(opusdepay, opusdec, audioconvert, audioresample, audiosink, NULL)) {
			ofLogError(LOG_NAME) << "couldn't link audio elements";
		}
	}
}


void ofxGstRTP::addVideoChannel(int port, string codec) {

	// the caps of the sender RTP stream.
	// FIXME: This is usually negotiated out of band with
	// SDP or RTSP. normally these caps will also include SPS and PPS but we don't
	// have that yet
	string vcaps;
	int iCodec;

	if (codec == "h264") {
		vcaps = "application/x-rtp,media=(string)video,clock-rate=(int)90000,payload=(int)96,encoding-name=(string)H264,rtcp-fb-nack-pli=(int)1";
		iCodec = 0;
	}
	else if (codec == "h265") {
		vcaps = "application/x-rtp,media=(string)video,clock-rate=(int)90000,payload=(int)96,encoding-name=(string)H265,rtcp-fb-nack-pli=(int)1";
		iCodec = 1;
	}

	createVideoChannel(vcaps, iCodec);

	GstElement* rtcpsink;
	NetworkElementsProperties properties;
	properties.capsstr = vcaps;
	properties.source = &vudpsrc;
	properties.rtpcsource = &vudpsrcrtcp;
	properties.rtpcsink = &rtcpsink;
	properties.port = port;
	properties.rtpcsrcport = port + 1;
	properties.rtpcsinkport = port + 3;
	properties.srcIP = src;
	properties.sessionNumber = videoSessionNumber;
	properties.sourceName = "vrtpsrc";
	properties.rtpcSourceName = "vrtcpsrc";
	properties.rtpcSinkName = "vrtcpsink";

	createNetworkElements(properties, NULL);
}


void ofxGstRTP::isRecordVideo(bool bRecord, string filePath) {
	recordVideo = bRecord;
	recordVideoPath = filePath;
}


void ofxGstRTP::addAudioChannel(int port) {

	// the caps of the sender RTP stream.
	// FIXME: This is usually negotiated out of band with
	// SDP or RTSP. normally these caps will also include SPS and PPS but we don't
	// have that yet
	string acaps = "application/x-rtp,media=(string)audio,clock-rate=(int)48000,payload=(int)97,encoding-name=(string)X-GST-OPUS-DRAFT-SPITTKA-00";

	createAudioChannel(acaps);

	GstElement* rtcpsink;
	NetworkElementsProperties properties;
	properties.capsstr = acaps;
	properties.source = &audpsrc;
	properties.rtpcsource = &audpsrcrtcp;
	properties.rtpcsink = &rtcpsink;
	properties.port = port;
	properties.rtpcsrcport = port + 1;
	properties.rtpcsinkport = port + 3;
	properties.srcIP = src;
	properties.sessionNumber = audioSessionNumber;
	properties.sourceName = "artpsrc";
	properties.rtpcSourceName = "artcpsrc";
	properties.rtpcSinkName = "artcpsink";

	createNetworkElements(properties, NULL);

}

void ofxGstRTP::setup(string srcIP, int latency) {
	this->src = srcIP;
	this->latency = latency;

	pipeline = gst_pipeline_new("rtpclientpipeline");
	if (!pipeline) {
		ofLogError() << "couldn't create pipeline";
	}
	gst.setSinkListener(this);

	rtpbin = gst_element_factory_make("rtpbin", "rtpbinclient");
	if (!rtpbin) {
		ofLogError() << "couldn't create rtpbin";
	}
	g_object_set(rtpbin, "latency", RTPBIN_MAX_LATENCY, NULL);
	g_object_set(rtpbin, "drop-on-latency", (bool)drop, NULL);
	g_object_set(rtpbin, "do-lost", TRUE, NULL);
	g_object_set(rtpbin, "message-forward", TRUE, NULL);

	if (!gst_bin_add(GST_BIN(pipeline), rtpbin)) {
		ofLogError() << "couldn't add rtpbin to pipeline";
	}
}

void ofxGstRTP::close() {
	gst_element_send_event(pipeline, gst_event_new_eos());
	gst_element_send_event(vmux, gst_event_new_eos());
	gst_element_send_event(queue1, gst_event_new_eos());
	gst_element_send_event(tee, gst_event_new_eos());

	gst.close();
	pipeline = 0;
	rtpbin = 0;
	rtpVideodepay = 0;
	tee = 0;
	parse = 0;
	vmux = 0;
	queue1 = 0;
	queue2 = 0;
	fileSink = 0;
	opusdepay = 0;
	gstdepay = 0;
	videoSink = 0;
	vudpsrc = 0;
	audpsrc = 0;
	vudpsrcrtcp = 0;
	audpsrcrtcp = 0;
	videoSessionNumber = -1;
	audioSessionNumber = -1;
	videoSSRC = 0;
	audioSSRC = 0;
	videoReady = false;
	audioReady = false;
	lastSessionNumber = 0;
}

void ofxGstRTP::requestKeyFrame() {
	GstClock* clock = gst_pipeline_get_clock(GST_PIPELINE(gst.getPipeline()));
	gst_object_ref(clock);
	GstClockTime time = gst_clock_get_time(clock);
	GstClockTime now = time - gst_element_get_base_time(gst.getPipeline());
	gst_object_unref(clock);
	GstEvent* keyFrameEvent = gst_video_event_new_upstream_force_key_unit(now, TRUE, 0);
	gst_element_send_event(gst.getPipeline(), keyFrameEvent);
}

void ofxGstRTP::latencyChanged(int& latency) {
	if (gst.isLoaded()) {
		g_object_set(rtpbin, "latency", latency, NULL);
		if (gst.isPlaying()) {
			gst_element_set_state(gst.getPipeline(), GST_STATE_PLAYING);
			requestKeyFrame();
			g_signal_emit_by_name(rtpbin, "reset-sync", NULL);
		}
	}
}

void ofxGstRTP::dropChanged(bool& drop) {
	g_object_set(rtpbin, "drop-on-latency", drop, NULL);
}

void ofxGstRTP::play() {
	// pass the pipeline to ofGstVideoUtils so it starts it and allocates the needed resources
	gst.setPipelineWithSink(pipeline, NULL, true);

	// connect callback to the on-ssrc-active signal
	g_signal_connect(gst.getGstElementByName("rtpbinclient"), "pad-added", G_CALLBACK(&ofxGstRTP::on_pad_added), this);
	g_signal_connect(gst.getGstElementByName("rtpbinclient"), "on-ssrc-active", G_CALLBACK(&ofxGstRTP::on_ssrc_active_handler), this);
	g_signal_connect(gst.getGstElementByName("rtpbinclient"), "on-bye-ssrc", G_CALLBACK(&ofxGstRTP::on_bye_ssrc_handler), this);
	g_signal_connect(gst.getGstElementByName("rtpbinclient"), "on-new-ssrc", G_CALLBACK(&ofxGstRTP::on_new_ssrc_handler), this);
	g_signal_connect(gst.getGstElementByName("rtpbinclient"), "on-sender-timeout", G_CALLBACK(&ofxGstRTP::on_sender_timeout_handler), this);

	if (audioechosrc) {
		gst_app_src_set_stream_type((GstAppSrc*)audioechosrc, GST_APP_STREAM_TYPE_STREAM);
	}

	gst.startPipeline();
	gst.play();
}


void ofxGstRTP::update() {
	doubleBufferVideo.update();
}


bool ofxGstRTP::isFrameNewVideo() {
	return doubleBufferVideo.isFrameNew();
}


ofPixels& ofxGstRTP::getPixelsVideo() {
	return doubleBufferVideo.getPixels();
}


bool ofxGstRTP::getClientSenderTimeout() {
	return bClientSenderTimeout;
}

bool ofxGstRTP::on_message(GstMessage* msg) {
	// read messages from the pipeline like dropped packages	

	switch (GST_MESSAGE_TYPE(msg)) {
	case GST_MESSAGE_EOS: {
		return true;
	}

	case GST_MESSAGE_ELEMENT: {
		GstObject* messageSrc = GST_MESSAGE_SRC(msg);
		ofLogVerbose(LOG_NAME) << "Got " << GST_MESSAGE_TYPE_NAME(msg) << " message from " << GST_MESSAGE_SRC_NAME(msg);
		ofLogVerbose(LOG_NAME) << "Message source type: " << G_OBJECT_CLASS_NAME(G_OBJECT_GET_CLASS(messageSrc));
		ofLogVerbose(LOG_NAME) << "With structure name: " << gst_structure_get_name(gst_message_get_structure(msg));
		ofLogVerbose(LOG_NAME) << gst_structure_to_string(gst_message_get_structure(msg));
		return true;
	}
	case GST_MESSAGE_QOS: {
		GstObject* messageSrc = GST_MESSAGE_SRC(msg);
		ofLogVerbose(LOG_NAME) << "Got " << GST_MESSAGE_TYPE_NAME(msg) << " message from " << GST_MESSAGE_SRC_NAME(msg);
		ofLogVerbose(LOG_NAME) << "Message source type: " << G_OBJECT_CLASS_NAME(G_OBJECT_GET_CLASS(messageSrc));

		GstFormat format;
		guint64 processed;
		guint64 dropped;
		gst_message_parse_qos_stats(msg, &format, &processed, &dropped);
		ofLogVerbose(LOG_NAME) << "format " << gst_format_get_name(format) << " processed " << processed << " dropped " << dropped;

		gint64 jitter;
		gdouble proportion;
		gint quality;
		gst_message_parse_qos_values(msg, &jitter, &proportion, &quality);
		ofLogVerbose(LOG_NAME) << "jitter " << jitter << " proportion " << proportion << " quality " << quality;

		gboolean live;
		guint64 running_time;
		guint64 stream_time;
		guint64 timestamp;
		guint64 duration;
		gst_message_parse_qos(msg, &live, &running_time, &stream_time, &timestamp, &duration);
		ofLogVerbose(LOG_NAME) << "live stream " << live << " runninng_time " << running_time << " stream_time " << stream_time << " timestamp " << timestamp << " duration " << duration;

		return true;
	}
	default:
		ofLogVerbose(LOG_NAME) << "Got " << GST_MESSAGE_TYPE_NAME(msg) << " message from " << GST_MESSAGE_SRC_NAME(msg);
		return false;
	}
}

void ofxGstRTP::on_eos() {
	disconnectedEvent.notify(this);
}

void ofxGstRTP::on_stream_prepared() {
};


void ofxGstRTP::on_eos_from_video(GstAppSink* elt, void* rtpClient) {
}


GstFlowReturn ofxGstRTP::on_new_preroll_from_video(GstAppSink* elt, void* rtpClient) {
	return GST_FLOW_OK;
}


GstFlowReturn ofxGstRTP::on_new_buffer_from_video(GstAppSink* elt, void* data) {
	ofxGstRTP* rtpClient = (ofxGstRTP*)data;
	return rtpClient->on_new_buffer_from_video(elt);
}


GstFlowReturn ofxGstRTP::on_new_buffer_from_video(GstAppSink* elt) {
	GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(elt));
	if (!doubleBufferVideo.isAllocated()) {
		GstCaps* sampleCaps = gst_sample_get_caps(sample);
		if (sampleCaps) {
			GstVideoInfo sampleInfo;
			if (gst_video_info_from_caps(&sampleInfo, sampleCaps)) {
				doubleBufferVideo.setup(sampleInfo.width, sampleInfo.height, 3);
			}
		}
	}
	if (doubleBufferVideo.isAllocated()) {
		doubleBufferVideo.newSample(sample);
	}
	return GST_FLOW_OK;
}