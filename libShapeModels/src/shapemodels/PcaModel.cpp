/*
 * PcaModel.cpp
 *
 *  Created on: 30.09.2013
 *      Author: Patrik Huber
 */

#include "shapemodels/PcaModel.hpp"

#include "logging/LoggerFactory.hpp"

#include "H5Cpp.h"

#include "boost/lexical_cast.hpp"
#include "boost/algorithm/string.hpp"

#include <fstream>

using logging::LoggerFactory;
using cv::Mat;
using cv::Vec3f;
using boost::lexical_cast;
using std::string;
using std::vector;
using std::array;

namespace shapemodels {

PcaModel::PcaModel()
{
	engine.seed();
}

PcaModel PcaModel::loadStatismoModel(string h5file, PcaModel::ModelType modelType)
{
	logging::Logger logger = Loggers->getLogger("shapemodels");
	PcaModel model;

	// Load the shape or color model from the .h5 file
	string h5GroupType;
	if (modelType == ModelType::SHAPE) {
		h5GroupType = "shape";
	} else if (modelType == ModelType::COLOR) {
		h5GroupType = "color";
	}

	H5::H5File h5Model;

	try {
		h5Model = H5::H5File(h5file, H5F_ACC_RDONLY);
	}
	catch (H5::Exception& e) {
		string errorMessage = "Could not open HDF5 file: " + string(e.getCDetailMsg());
		logger.error(errorMessage);
		throw errorMessage;
	}

	// Load either the shape or texture mean
	string h5Group = "/" + h5GroupType + "/model";
	H5::Group modelReconstructive = h5Model.openGroup(h5Group);

	// Read the mean
	H5::DataSet dsMean = modelReconstructive.openDataSet("./mean");
	hsize_t dims[2];
	dsMean.getSpace().getSimpleExtentDims(dims, NULL);	// dsMean.getSpace() leaks memory... maybe a hdf5 bug, maybe vlenReclaim(...) could be a fix. No idea.
	//H5::DataSpace dsp = dsMean.getSpace();
	//dsp.close();
	Loggers->getLogger("shapemodels").debug("Dimensions of the model mean: " + lexical_cast<string>(dims[0]));
	model.mean = Mat(1, dims[0], CV_32FC1); // Use a row-vector, because of faster memory access and I'm not sure the memory block is allocated contiguously if we have multiple rows.
	dsMean.read(model.mean.ptr<float>(0), H5::PredType::NATIVE_FLOAT);
	model.mean = model.mean.t(); // Transpose it to a col-vector
	dsMean.close();

	// Read the eigenvalues
	dsMean = modelReconstructive.openDataSet("./pcaVariance");
	dsMean.getSpace().getSimpleExtentDims(dims, NULL);
	Loggers->getLogger("shapemodels").debug("Dimensions of the pcaVariance: " + lexical_cast<string>(dims[0]));
	model.eigenvalues = Mat(1, dims[0], CV_32FC1);
	dsMean.read(model.eigenvalues.ptr<float>(0), H5::PredType::NATIVE_FLOAT);
	model.eigenvalues = model.eigenvalues.t();
	dsMean.close();

	// Read the PCA basis matrix
	dsMean = modelReconstructive.openDataSet("./pcaBasis");
	dsMean.getSpace().getSimpleExtentDims(dims, NULL);
	Loggers->getLogger("shapemodels").debug("Dimensions of the PCA basis matrix: " + lexical_cast<string>(dims[0]) + ", " + lexical_cast<string>(dims[1]));
	model.pcaBasis = Mat(dims[0], dims[1], CV_32FC1);
	dsMean.read(model.pcaBasis.ptr<float>(0), H5::PredType::NATIVE_FLOAT);
	dsMean.close();

	modelReconstructive.close(); // close the model-group

	// Read the noise variance (not implemented)
	/*dsMean = modelReconstructive.openDataSet("./noiseVariance");
	float noiseVariance = 10.0f;
	dsMean.read(&noiseVariance, H5::PredType::NATIVE_FLOAT);
	dsMean.close(); */

	// Read the triangle-list
	string representerGroupName = "/" + h5GroupType + "/representer";
	H5::Group representerGroup = h5Model.openGroup(representerGroupName);
	dsMean = representerGroup.openDataSet("./reference-mesh/triangle-list");
	dsMean.getSpace().getSimpleExtentDims(dims, NULL);
	Loggers->getLogger("shapemodels").debug("Dimensions of the triangle-list: " + lexical_cast<string>(dims[0]) + ", " + lexical_cast<string>(dims[1]));
	Mat triangles(dims[0], dims[1], CV_32SC1);
	dsMean.read(triangles.ptr<int>(0), H5::PredType::NATIVE_INT32);
	dsMean.close();
	representerGroup.close();
	model.triangleList.resize(triangles.rows);
	for (unsigned int i = 0; i < model.triangleList.size(); ++i) {
		model.triangleList[i][0] = triangles.at<int>(i, 0);
		model.triangleList[i][1] = triangles.at<int>(i, 1);
		model.triangleList[i][2] = triangles.at<int>(i, 2);
	}

	// Load the landmarks mappings:
	// load the reference-mesh
	representerGroup = h5Model.openGroup(representerGroupName);
	dsMean = representerGroup.openDataSet("./reference-mesh/vertex-coordinates");
	dsMean.getSpace().getSimpleExtentDims(dims, NULL);
	Loggers->getLogger("shapemodels").debug("Dimensions of the reference-mesh vertex-coordinates matrix: " + lexical_cast<string>(dims[0]) + ", " + lexical_cast<string>(dims[1]));
	Mat referenceMesh(dims[0], dims[1], CV_32FC1);
	dsMean.read(referenceMesh.ptr<float>(0), H5::PredType::NATIVE_FLOAT);
	dsMean.close();
	representerGroup.close();

	// convert to 3 vectors with the x, y and z coordinates for easy searching
	vector<float> refx(referenceMesh.col(0).clone());
	vector<float> refy(referenceMesh.col(1).clone());
	vector<float> refz(referenceMesh.col(2).clone());

	// load the landmarks info (mapping name <-> reference (x, y, z)-coords)
	H5::Group landmarksGroup = h5Model.openGroup("/metadata/landmarks");
	dsMean = landmarksGroup.openDataSet("./text");
	
	H5std_string outputString;
	Loggers->getLogger("shapemodels").debug("Reading landmark information from the model.");
	dsMean.read(outputString, dsMean.getStrType());
	dsMean.close();
	landmarksGroup.close();
	vector<string> landmarkLines;
	boost::split(landmarkLines, outputString, boost::is_any_of("\n"), boost::token_compress_on);
	for (const auto& l : landmarkLines) {
		if (l == "") {
			continue;
		}
		vector<string> line;
		boost::split(line, l, boost::is_any_of(" "), boost::token_compress_on);
		string name = line[0];
		int visibility = lexical_cast<int>(line[1]);
		float x = lexical_cast<float>(line[2]);
		float y = lexical_cast<float>(line[3]);
		float z = lexical_cast<float>(line[4]);
		// Find the x, y and z values in the reference
		const auto ivx = std::find(begin(refx), end(refx), x);
		const auto ivy = std::find(begin(refy), end(refy), y);
		const auto ivz = std::find(begin(refz), end(refz), z);
		// TODO Check for .end()!
		const auto vertexIdX = std::distance(begin(refx), ivx);
		const auto vertexIdY = std::distance(begin(refy), ivy);
		const auto vertexIdZ = std::distance(begin(refz), ivz);
		// assert vx=vy=vz
		// Hmm this is not perfect. If there's another vertex where 1 or 2 coords are the same, it fails.
		// We should do the search differently: Find _all_ the vertices that are equal, then take the one that has the right x, y and z.
		model.landmarkVertexMap.insert(make_pair(name, vertexIdX));
	
	}

	h5Model.close();
	return model;
}

PcaModel PcaModel::loadScmModel(string modelFilename, string landmarkVertexMappingFile, PcaModel::ModelType modelType)
{
	logging::Logger logger = Loggers->getLogger("shapemodels");
	PcaModel model;

	// Load the landmarks mappings
	std::ifstream ffpList;
	ffpList.open(landmarkVertexMappingFile.c_str(), std::ios::in);
	if (!ffpList.is_open()) {
		string errorMessage = "Error opening feature points file " + landmarkVertexMappingFile + ".";
		logger.error(errorMessage);
		throw std::runtime_error(errorMessage);
	}
	string line;
	while (ffpList.good()) {
		std::getline(ffpList, line);
		if (line == "" || line.substr(0, 2) == "//") { // empty line or starting with a '//'
			continue;
		}
		string currFfp; // Have a buffer string
		int currVertex = 0;
		std::stringstream ss(line); // Insert the string into a stream
		ss >> currFfp;
		ss >> currVertex;
		model.landmarkVertexMap.insert(make_pair(currFfp, currVertex));
		currFfp.clear();
	}
	ffpList.close();

	// Load the model
	if (sizeof(unsigned int) != 4) {
		logger.warn("Warning: We're reading 4 Bytes from the file but sizeof(unsigned int) != 4. Check the code/behaviour.");
	}
	if (sizeof(double) != 8) {
		logger.warn("Warning: We're reading 8 Bytes from the file but sizeof(double) != 8. Check the code/behaviour.");
	}

	std::ifstream modelFile;
	modelFile.open(modelFilename, std::ios::binary);
	if (!modelFile.is_open()) {
		logger.warn("Could not open model file: " + modelFilename);
		exit(EXIT_FAILURE);
	}

	// Reading the shape model
	// Read (reference?) num triangles and vertices
	unsigned int numVertices = 0;
	unsigned int numTriangles = 0;
	modelFile.read(reinterpret_cast<char*>(&numVertices), 4); // 1 char = 1 byte. uint32=4bytes. float64=8bytes.
	modelFile.read(reinterpret_cast<char*>(&numTriangles), 4);

	// Read triangles
	vector<array<int, 3>> triangleList;

	triangleList.resize(numTriangles);
	unsigned int v0, v1, v2;
	for (unsigned int i=0; i < numTriangles; ++i) {
		v0 = v1 = v2 = 0;
		modelFile.read(reinterpret_cast<char*>(&v0), 4);	// would be nice to pass a &vector and do it in one
		modelFile.read(reinterpret_cast<char*>(&v1), 4);	// go, but didn't work. Maybe a cv::Mat would work?
		modelFile.read(reinterpret_cast<char*>(&v2), 4);
		triangleList[i][0] = v0;
		triangleList[i][1] = v1;
		triangleList[i][2] = v2;
	}

	// Read number of rows and columns of the shape projection matrix (pcaBasis)
	unsigned int numShapePcaCoeffs = 0;
	unsigned int numShapeDims = 0;	// dimension of the shape vector (3*numVertices)
	modelFile.read(reinterpret_cast<char*>(&numShapePcaCoeffs), 4);
	modelFile.read(reinterpret_cast<char*>(&numShapeDims), 4);

	if (3*numVertices != numShapeDims) {
		logger.warn("Warning: Number of shape dimensions is not equal to three times the number of vertices. Something will probably go wrong during the loading.");
	}

	// Read shape projection matrix
	Mat pcaBasisShape(numShapeDims, numShapePcaCoeffs, CV_32FC1); // m x n (rows x cols) = numShapeDims x numShapePcaCoeffs
	logger.debug("Loading PCA basis matrix with " + lexical_cast<string>(pcaBasisShape.rows) + " rows and " + lexical_cast<string>(pcaBasisShape.cols) + "cols.");
	for (unsigned int col = 0; col < numShapePcaCoeffs; ++col) {
		for (unsigned int row = 0; row < numShapeDims; ++row) {
			double var = 0.0;
			modelFile.read(reinterpret_cast<char*>(&var), 8);
			pcaBasisShape.at<float>(row, col) = static_cast<float>(var);
		}
	}

	// Read mean shape vector
	unsigned int numMean = 0; // dimension of the mean (3*numVertices)
	modelFile.read(reinterpret_cast<char*>(&numMean), 4);
	if (numMean != numShapeDims) {
		logger.warn("Warning: Number of shape dimensions is not equal to the number of dimensions of the mean. Something will probably go wrong during the loading.");
	}
	Mat meanShape(numMean, 1, CV_32FC1);
	unsigned int counter = 0;
	double vd0, vd1, vd2;
	for (unsigned int i=0; i < numMean/3; ++i) {
		vd0 = vd1 = vd2 = 0.0;
		modelFile.read(reinterpret_cast<char*>(&vd0), 8);
		modelFile.read(reinterpret_cast<char*>(&vd1), 8);
		modelFile.read(reinterpret_cast<char*>(&vd2), 8);
		meanShape.at<float>(counter, 0) = static_cast<float>(vd0);
		++counter;
		meanShape.at<float>(counter, 0) = static_cast<float>(vd1);
		++counter;
		meanShape.at<float>(counter, 0) = static_cast<float>(vd2);
		++counter;
	}

	// Read shape eigenvalues
	unsigned int numEigenValsShape = 0;
	modelFile.read(reinterpret_cast<char*>(&numEigenValsShape), 4);
	if (numEigenValsShape != numShapePcaCoeffs) {
		logger.warn("Warning: Number of coefficients in the PCA basis matrix is not equal to the number of eigenvalues. Something will probably go wrong during the loading.");
	}
	Mat eigenvaluesShape(numEigenValsShape, 1, CV_32FC1);
	for (unsigned int i=0; i < numEigenValsShape; ++i) {
		double var = 0.0;
		modelFile.read(reinterpret_cast<char*>(&var), 8);
		eigenvaluesShape.at<float>(i, 0) = static_cast<float>(var);
	}

	if (modelType == ModelType::SHAPE) {
		model.mean = meanShape;
		model.pcaBasis = pcaBasisShape;
		model.eigenvalues = eigenvaluesShape;
		model.triangleList = triangleList;

		modelFile.close();

		return model;
	}

	// Reading the color model
	// Read number of rows and columns of projection matrix
	unsigned int numTexturePcaCoeffs = 0;
	unsigned int numTextureDims = 0;
	modelFile.read(reinterpret_cast<char*>(&numTexturePcaCoeffs), 4);
	modelFile.read(reinterpret_cast<char*>(&numTextureDims), 4);
	// Read color projection matrix
	Mat pcaBasisColor(numTextureDims, numTexturePcaCoeffs, CV_32FC1);
	logger.debug("Loading PCA basis matrix with " + lexical_cast<string>(pcaBasisShape.rows) + " rows and " + lexical_cast<string>(pcaBasisShape.cols) + "cols.");
	for (unsigned int col = 0; col < numTexturePcaCoeffs; ++col) {
		for (unsigned int row = 0; row < numTextureDims; ++row) {
			double var = 0.0;
			modelFile.read(reinterpret_cast<char*>(&var), 8);
			pcaBasisColor.at<float>(row, col) = static_cast<float>(var);
		}
	}

	// Read mean color vector
	unsigned int numMeanColor = 0; // dimension of the mean (3*numVertices)
	modelFile.read(reinterpret_cast<char*>(&numMeanColor), 4);
	Mat meanColor(numMeanColor, 1, CV_32FC1);
	counter = 0;
	for (unsigned int i=0; i < numMeanColor/3; ++i) {
		vd0 = vd1 = vd2 = 0.0;
		modelFile.read(reinterpret_cast<char*>(&vd0), 8); // order in hdf5: RGB. Order in OCV: BGR. But order in vertex.color: RGB
		modelFile.read(reinterpret_cast<char*>(&vd1), 8);
		modelFile.read(reinterpret_cast<char*>(&vd2), 8);
		meanColor.at<float>(counter, 0) = static_cast<float>(vd0);
		++counter;
		meanColor.at<float>(counter, 0) = static_cast<float>(vd1);
		++counter;
		meanColor.at<float>(counter, 0) = static_cast<float>(vd2);
		++counter;
	}

	// Read color eigenvalues
	unsigned int numEigenValsColor = 0;
	modelFile.read(reinterpret_cast<char*>(&numEigenValsColor), 4);
	Mat eigenvaluesColor(numEigenValsColor, 1, CV_32FC1);
	for (unsigned int i=0; i < numEigenValsColor; ++i) {
		double var = 0.0;
		modelFile.read(reinterpret_cast<char*>(&var), 8);
		eigenvaluesColor.at<float>(i, 0) = static_cast<float>(var);
	}

	if (modelType == ModelType::COLOR) {
		model.mean = meanColor;
		model.pcaBasis = pcaBasisColor;
		model.eigenvalues = eigenvaluesColor;
		model.triangleList = triangleList;

		modelFile.close();

		return model;
	}

	logger.error("Unknown ModelType, should never reach here.");
	//modelFile.close();
	//return model;
}

unsigned int PcaModel::getNumberOfPrincipalComponents() const
{
	return pcaBasis.cols;
}


unsigned int PcaModel::getDataDimension() const
{
	return pcaBasis.rows;
}


std::vector<std::array<int, 3>> PcaModel::getTriangleList() const
{
	return triangleList;
}

Mat PcaModel::getMean() const
{
	return mean;
}

Vec3f PcaModel::getMeanAtPoint(string landmarkIdentifier) const
{
	int vertexId = landmarkVertexMap.at(landmarkIdentifier);
	vertexId *= 3;
	return Vec3f(mean.at<float>(vertexId), mean.at<float>(vertexId+1), mean.at<float>(vertexId+2)); // we could use Vec3f(mean(Range(), Range())), maybe then we don't copy the data?
}

Vec3f PcaModel::getMeanAtPoint(unsigned int vertexIndex) const
{
	vertexIndex *= 3;
	return Vec3f(mean.at<float>(vertexIndex), mean.at<float>(vertexIndex+1), mean.at<float>(vertexIndex+2));
}

Mat PcaModel::drawSample(float sigma /*= 1.0f*/)
{
	std::normal_distribution<float> distribution(0.0f, sigma); // TODO: c'tor takes the stddev. Update all the documentation!!!

	vector<float> alphas(getNumberOfPrincipalComponents());

	for (auto& a : alphas) {
		a = distribution(engine);
	}

	return drawSample(alphas);

	/* without calling drawSample(alphas): (maybe if we add noise)
	Mat alphas = Mat::zeros(getNumberOfPrincipalComponents(), 1, CV_32FC1);
	for (int row=0; row < alphas.rows; ++row) {
		alphas.at<float>(row, 0) = distribution(engine);
	}
	*/

	/* with noise: (does the noise make sense in drawSample(vector<float> coefficients)?)
	unsigned int vsize = mean.size();
	vector epsilon = Utils::generateNormalVector(vectorSize) * sqrt(m_noiseVariance);
	return m_mean + m_pcaBasisMatrix * coefficients + epsilon;
	*/
}

Mat PcaModel::drawSample(vector<float> coefficients)
{
	Mat alphas(coefficients);
	Mat sqrtOfEigenvalues = eigenvalues.clone();
	for (unsigned int i = 0; i < eigenvalues.rows; ++i)	{
		sqrtOfEigenvalues.at<float>(i) = std::sqrt(eigenvalues.at<float>(i));
	}

	//Mat smallBasis = pcaBasis(cv::Rect(0, 0, 55, 100));
	//Mat smallMean = mean(cv::Rect(0, 0, 1, 100));

	Mat modelSample = mean + pcaBasis * alphas.mul(sqrtOfEigenvalues); // Surr
	//Mat modelSample = mean + pcaBasis * alphas; // Bsl .h5 old

	return modelSample;
}

cv::Mat PcaModel::getPcaBasis() const
{
	return pcaBasis;
}

cv::Mat PcaModel::getPcaBasis(std::string landmarkIdentifier) const
{
	int vertexId = landmarkVertexMap.at(landmarkIdentifier);
	vertexId *= 3;
	return pcaBasis.rowRange(vertexId, vertexId + 3);
}

}