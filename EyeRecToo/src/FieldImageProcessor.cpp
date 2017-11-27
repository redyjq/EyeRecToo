#include "FieldImageProcessor.h"
#include <opencv2/highgui.hpp>

using namespace std;
using namespace cv;
using namespace aruco;

static int gFieldDataId = qRegisterMetaType<FieldData>("FieldData");

FieldImageProcessor::FieldImageProcessor(QString id, QObject *parent)
    : id(id),
      sROI(QPointF(0,0)),
      eROI(QPointF(1,1)),
      forceSanitize(false),
      QObject(parent)
{
    settings = new QSettings(gCfgDir + "/" + id + " ImageProcessor", QSettings::IniFormat);
    updateConfig();

    dict = getPredefinedDictionary(DICT_4X4_250);
    detectorParameters = new DetectorParameters();
    detectorParameters->markerBorderBits = 2;
    detectorParameters->minMarkerPerimeterRate = 0.10; // TODO: determine a good value for these based on the fov and maximum detection distance
    //printMarkers(); // TODO: parametrize me

    pmIdx = gPerformanceMonitor.enrol(id, "Image Processor");
}

void FieldImageProcessor::updateConfig()
{
    QMutexLocker locker(&cfgMutex);
    cfg.load(settings);
    forceSanitize = true;
}

FieldImageProcessor::~FieldImageProcessor()
{
    if(settings)
        settings->deleteLater();
}

void FieldImageProcessor::process(Timestamp timestamp, const Mat &frame)
{
    // TODO: parametrize frame drop due to lack of processing power
    if ( gPerformanceMonitor.shouldDrop(pmIdx, gTimer.elapsed() - timestamp, 100) )
        return;

    QMutexLocker locker(&cfgMutex);

    data.timestamp = timestamp;

    if (cfg.inputSize.width > 0 && cfg.inputSize.height > 0) {
        data.input = Mat(cfg.inputSize, frame.type() );
        resize(frame, data.input, cfg.inputSize);
    }
	else {
		Q_ASSERT_X(frame.data != data.input.data, "Field Image Processing", "Previous and current input image matches!");
		data.input = frame;
	}

    if (cfg.flip != CV_FLIP_NONE)
        flip(data.input, data.input, cfg.flip);

    sanitizeCameraParameters( Size(data.input.cols, data.input.rows) );

    data.undistorted = cfg.undistort;
    if (data.undistorted)
        remap(data.input, data.input, map1, map2, CV_INTER_AREA);

    data.width = data.input.cols;
    data.height = data.input.rows;

    // Marker detection and pose estimation
    vector<int> ids;
    vector<vector<Point2f> > corners;
	Mat downscaled;
	if (cfg.processingDownscalingFactor > 1) {
        resize(data.input, downscaled, Size(),
               1/cfg.processingDownscalingFactor,
               1/cfg.processingDownscalingFactor,
               INTER_AREA);
	} else {
		downscaled = data.input;
	}

	if (cfg.markerDetectionMethod == "aruco" || gCalibrating) {

		detectMarkers(downscaled, dict, corners, ids, detectorParameters);

		if (cfg.processingDownscalingFactor > 1) { // Upscale if necessary
			for (unsigned int i=0; i<ids.size(); i++)
				for (unsigned int j=0; j<corners[i].size(); j++)
					corners[i][j] = cfg.processingDownscalingFactor*corners[i][j];
		}
	}

    // Filling the marker data
    data.collectionMarker = Marker();
    data.markers.clear();

	// Note that the following is based on the COLLECTION MARKER size.
	/* TODO: check whether the pose estimation works with fisheye intrinsic parameters
	 * An initial (and short) test with a pupil labs wide angle camera at 720p seeemed
	 * to match the distance measured with a laser distance meter.
	 */
    if (ids.size() > 0) {
        if (data.undistorted) {
            Mat emptyCameraMatrix = (Mat_<double>(3,3) <<
                           data.width,          0, data.width/2,
                           0         , data.width, data.height/2,
                           0         ,0          , 1);
            Mat emptyDistCoeffs = (Mat_<double>(1,4) << 0, 0, 0, 0);
            estimatePoseSingleMarkers(corners, cfg.collectionMarkerSizeMeters, emptyCameraMatrix, emptyDistCoeffs, rvecs, tvecs);
        } else
			estimatePoseSingleMarkers(corners, cfg.collectionMarkerSizeMeters, cameraMatrix, distCoeffs, rvecs, tvecs);
    }

    for (unsigned int i=0; i<ids.size(); i++) {
        Marker marker(corners[i], ids[i]);

        marker.center = estimateMarkerCenter(marker.corners);
        marker.center.z = tvecs.at<double>(i,2);
        marker.tv = ( Mat_<float>(1,3) << tvecs.at<double>(i,0), tvecs.at<double>(i,1), tvecs.at<double>(i,2) );
        marker.rv = ( Mat_<float>(1,3) << rvecs.at<double>(i,0), rvecs.at<double>(i,1), rvecs.at<double>(i,2) );

        data.markers.push_back( marker );

        // use closest calibration marker -- to try and avoid detecting the one viewed in the field camera when testing :-)
        if (marker.id == cfg.collectionMarkerId) {
            if (data.collectionMarker.id == -1)
                data.collectionMarker = data.markers.back();
            else if (data.collectionMarker.center.z > marker.center.z)
                data.collectionMarker = data.markers.back();
        }
    }

    data.validGazeEstimate = false;
	data.processingTimestamp = gTimer.elapsed() - data.timestamp;

    emit newData(data);
}

void FieldImageProcessor::newROI(QPointF sROI, QPointF eROI)
{
    QMutexLocker locker(&cfgMutex);
    if (sROI.isNull() || eROI.isNull()) {
        this->sROI = QPointF(0,0);
        this->eROI = QPointF(1,1);
    } else {
        this->sROI = sROI;
        this->eROI = eROI;
    }
}

void FieldImageProcessor::sanitizeCameraParameters(Size size)
{
    if (!forceSanitize) {
        if ( (size == imageSize) && !map1.empty() && !map2.empty() )
            return;
    } else
        forceSanitize = false;

	/* TODO:
	 *
	 * 1) Move camera parameters logic to the Camera instead so it's
	 * considered for the eye cameras as well.
	 *
	 * 2) Drop the logic for resizing the intrinsic parameters since it's not
	 * throughly tested.
	 *
	 */

    FileStorage fs( QString(gCfgDir + "/" + id + "Calibration.xml").toStdString(), FileStorage::READ);
    fs["cameraMatrix"] >> cameraMatrix;
	fs["distCoeffs"] >> distCoeffs;
	fs["imageSize"] >> imageSize;
	bool haveCameraParameters = false;
    if (!cameraMatrix.empty() && !distCoeffs.empty())
        haveCameraParameters = true;

    double currentAspectRatio = size.width / (float) size.height;
    double originalAspectRatio = imageSize.width / (float) imageSize.height;

    if (haveCameraParameters && (currentAspectRatio == originalAspectRatio) ) {
        qInfo() << "Found intrinsic parameters.";
        double rx = imageSize.width  / (double) size.width;
        double ry = imageSize.height / (double) size.height;
        cameraMatrix.at<double>(0,2) = cameraMatrix.at<double>(0,2) / rx;
        cameraMatrix.at<double>(1,2) = cameraMatrix.at<double>(1,2) / ry;
        cameraMatrix.at<double>(0,0) = cameraMatrix.at<double>(0,0) / rx;
        cameraMatrix.at<double>(1,1) = cameraMatrix.at<double>(1,1) / rx;
        imageSize = size;
    } else {
        qInfo() << "No valid intrinsic parameters available. Using dummy values";
        // Dummy estimation
        cameraMatrix = (Mat_<double>(3,3) <<
		                size.width, 0, 0.5*size.width,
		                0, size.width, 0.5*size.height,
                        0, 0 ,1 );
        distCoeffs = (Mat_<double>(1,4) << 0, 0, 0, 0);
        imageSize = size;
    }

    initUndistortRectifyMap(
                            cameraMatrix,
                            distCoeffs,
                            Mat(),
							getOptimalNewCameraMatrix(cameraMatrix, distCoeffs, imageSize, 1, imageSize),
                            imageSize,
                            CV_32FC1,
                            map1,
                            map2
                            );
}
