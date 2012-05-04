#include "PoseGraph.hpp"

namespace graph_slam 
{

Transformation3 eigen2Hogman( const Eigen::Affine3d& eigen_transform )
{
    Eigen::Quaternionf eigen_quat(eigen_transform.rotation().cast<float>());
    Eigen::Matrix4d eigen_mat( eigen_transform.matrix() );
    Vector3 translation(eigen_mat(0, 3), eigen_mat(1, 3), eigen_mat(2, 3));
    Quaternion rotation(eigen_quat.x(), eigen_quat.y(), eigen_quat.z(),
	    eigen_quat.w());
    Transformation3 result(translation, rotation);

    return result;
}

Matrix6 eigen2Hogman( const Eigen::Matrix<double,6,6>& eigen_matrix )
{
    Matrix6 result;
    // TODO might be more efficient to perform a memcopy here
    for( int m=0; m<6; m++ )
	for( int n=0; n<6; n++ )
	    result[m][n] = eigen_matrix(m,n);

    return result;
}

Eigen::Matrix<double,6,6> hogman2Eigen( const Matrix6& hogman )
{
    Eigen::Matrix<double,6,6> result;

    for( int m=0; m<6; m++ )
	for( int n=0; n<6; n++ )
	    result(m,n) = hogman[m][n];

    return result;
}

Eigen::Affine3d hogman2Eigen( const Transformation3& hogman_transform )
{
    Eigen::Quaterniond rotation( 
	    hogman_transform.rotation().w(),
	    hogman_transform.rotation().x(),
	    hogman_transform.rotation().y(),
	    hogman_transform.rotation().z() );

    Eigen::Translation3d translation(
	    hogman_transform.translation().x(),
	    hogman_transform.translation().y(),
	    hogman_transform.translation().z() );

    return translation * rotation;
}

/** 
 * transform a 6x6 covariance matrix in [r t] order to a hogman 6 inverse
 * covariance matrix in [t r] format.
 */
Matrix6 envireCov2HogmanInf( const Eigen::Matrix<double,6,6>& eigen_matrix )
{
    Eigen::Matrix<double,6,6> t;
    t << eigen_matrix.bottomRightCorner<3,3>(), eigen_matrix.bottomLeftCorner<3,3>(),
      eigen_matrix.topRightCorner<3,3>(), eigen_matrix.topLeftCorner<3,3>();

    return eigen2Hogman( Eigen::Matrix<double,6,6>(t.inverse()) );
}

/** 
 * Transform a 6x6 covariance matrix in [t r] order from hogman Covariance
 * format to the envire [r t] order.
 */
Eigen::Matrix<double,6,6> hogmanCov2EnvireCov( const Matrix6& hogman_matrix )
{
    Eigen::Matrix<double,6,6> t = hogman2Eigen( hogman_matrix );
    Eigen::Matrix<double,6,6> t1;
    t1 << t.bottomRightCorner<3,3>(), t.bottomLeftCorner<3,3>(),
      t.topRightCorner<3,3>(), t.topLeftCorner<3,3>();
    return t1;
}

PoseGraph::PoseGraph( envire::Environment* env, int num_levels, int node_distance ) 
    : max_node_radius( 25.0 ), 
    min_sparse_correspondences( 7 ),
    env( env ), optimizer( new AISNavigation::HCholOptimizer3D( num_levels, node_distance ) ) 
{
    // set-up body frame
    bodyFrame = new envire::FrameNode();
    env->addChild( env->getRootNode(), bodyFrame.get() );

    // set-up chain for distance images
    distFrame = new envire::FrameNode();
    env->addChild( bodyFrame.get(), distFrame.get() );

    distOp = new envire::DistanceGridToPointcloud();
    distOp->setMaxDistance( 5.0 );
    env->attachItem( distOp.get() );

    // set-up the chain for the feature clouds
    featureFrame = new envire::FrameNode();
    env->addChild( bodyFrame.get(), featureFrame.get() );
    featurecloud = new envire::Featurecloud();
    env->setFrameNode( featurecloud.get(), featureFrame.get() );

    // set-up the target mls
    const double cell_size = 0.2;
    const double grid_size = 150;
    mlsGrid = new envire::MLSGrid( 
	    grid_size / cell_size, grid_size / cell_size, 
	    cell_size, cell_size, -grid_size / 2.0, -grid_size / 2.0 );
    mlsGrid->setHorizontalPatchThickness( 0.5 );
    mlsGrid->setGapSize( 1.0 );
    env->setFrameNode( mlsGrid.get(), env->getRootNode() );
    mlsOp = new envire::MLSProjection();
    mlsOp->useUncertainty( false );
    env->attachItem( mlsOp.get() );
    mlsOp->addOutput( mlsGrid.get() );
}

PoseGraph::~PoseGraph()
{
    // do some cleanup
    delete optimizer;

    // delete the nodeMap objects
    for( std::map<long, SensorMaps*>::iterator it = nodeMap.begin(); it != nodeMap.end(); it++ )
	delete it->second;
}

void PoseGraph::initNode( const envire::TransformWithUncertainty &body2bodyPrev, const envire::TransformWithUncertainty &body2world )
{
    // handle relative poses
    initNode( body2bodyPrev );

    // handle the root vertex, which should contain the rotation constraints given
    // by body2world
    AISNavigation::PoseGraph3D::Vertex *root_vertex = optimizer->vertex( 0 );
    if( !root_vertex )
    {
	// create a root node 
	root_vertex = optimizer->addVertex( 
		0,
		Transformation3(),
		Matrix6::eye(1.0) );
    }

    // create a copy of the body2world transform, and set the covariance
    // for the position very large
    envire::TransformWithUncertainty body2root = body2world;
    Eigen::Matrix<double,6,6> cov = body2root.getCovariance();
    cov.bottomRightCorner<3,3>() = Eigen::Vector3d( 1e8, 1e8, 1e8 ).asDiagonal();
    body2root.setCovariance( cov );

    optimizer->addEdge( 
	    root_vertex,
	    optimizer->vertex( currentBodyFrame->getUniqueIdNumericalSuffix() ),
	    eigen2Hogman( body2root.getTransform() ),
	    envireCov2HogmanInf( body2root.getCovariance() )
	    );
}

/** will prepare a new node based on an initial transformation
*/
void PoseGraph::initNode( const envire::TransformWithUncertainty &body2bodyPrev )
{
    // apply the relative transform to the current bodyFrame position
    bodyFrame->setTransform( bodyFrame->getTransformWithUncertainty() *
	    body2bodyPrev );

    // copy the bodyFrame node, and use it to store any additional map information
    // that is added through the other add functions
    currentBodyFrame = new envire::FrameNode(
	    bodyFrame->getTransformWithUncertainty() );

    env->addChild( bodyFrame->getParent(), currentBodyFrame.get() );

    // create a new hogman vertex for this node
    AISNavigation::PoseGraph3D::Vertex *new_vertex = optimizer->addVertex( 
	    currentBodyFrame->getUniqueIdNumericalSuffix(),
	    Transformation3(),
	    Matrix6::eye(1.0) );

    // in case there is a previous node create a vertex based on odometry
    // information
    if( prevBodyFrame )
    {
	optimizer->addEdge( 
		optimizer->vertex( prevBodyFrame->getUniqueIdNumericalSuffix() ),
		new_vertex,
		eigen2Hogman( body2bodyPrev.getTransform() ),
		envireCov2HogmanInf( body2bodyPrev.getCovariance() )
		);
    }
}

/** adds a sensor reading for a distance image to an initialized node
*/
void PoseGraph::addSensorReading( const base::samples::DistanceImage& distImage, const Eigen::Affine3d& sensor2body, const base::samples::frame::Frame& textureImage )
{
    // configure the processing chain for the distance image
    assert( currentBodyFrame );

    // create new distance grid object
    distFrame->setTransform( sensor2body );
    if( !distGrid )
    {
	// create new grid using the parameters from the distance image
	distGrid = new envire::DistanceGrid( distImage );

	// distGrid has just been created and needs to be attached
	distOp->addInput( distGrid.get() );
	distGrid->setFrameNode( distFrame.get() );
    }
    distGrid->copyFromDistanceImage( distImage );

    // create new imagegrid object if not available
    if( !textureGrid )
    {
	// copy the scaling properties from distanceImage
	// TODO this is a hack!
	textureGrid = new envire::ImageRGB24( 
		distImage.width, distImage.height, 
		distImage.scale_x, distImage.scale_y, 
		distImage.center_x, distImage.center_y );
	env->attachItem( textureGrid.get() );

	distOp->addInput( textureGrid.get() );
	textureGrid->setFrameNode( distFrame.get() );
    }
    textureGrid->copyFromFrame( textureImage );

    envire::Pointcloud::Ptr distPc = new envire::Pointcloud();
    env->setFrameNode( distPc.get(), currentBodyFrame.get() );
    distPc->setLabel("dense");
    distOp->removeOutputs();
    distOp->addOutput( distPc.get() );

    distOp->updateAll();

    mlsOp->addInput( distPc.get() );

    /*
       envire::GraphViz gv;
       gv.writeToFile( env, "/tmp/gv.dot" );
       */
}	

/** adds a sensor reading for a feature array to an initialized node
*/
void PoseGraph::addSensorReading( const stereo::StereoFeatureArray& featureArray, const Eigen::Affine3d& sensor2body )
{
    // configure the processing chain for the feature image 
    assert( currentBodyFrame );
    featureFrame->setTransform( sensor2body );
    // copy only up to a certain distance to omit the features further out
    featureArray.copyTo( *featurecloud.get(), 5.0 );

    envire::Featurecloud::Ptr featurePc = new envire::Featurecloud();
    env->setFrameNode( featurePc.get(), currentBodyFrame.get() );
    featurePc->setLabel("sparse");

    featurePc->copyFrom( featurecloud.get() );
    featurePc->itemModified();
}

/** adds an initialized node, with optional sensor readings to the node graph
 *
 * requires a previous call to initNode(), as well addSensorReading() calls
 * for each sensor reading.
 */
void PoseGraph::addNode()
{
    assert( currentBodyFrame );

    // find relevant associations based on the current state of the graph
    // for now, we just associate with the previous node. 
    //if( prevBodyFrame )
    //{
    //    associateNodes( prevBodyFrame.get(), currentBodyFrame.get() );
    //}

    // perform association with other nodes, to actually form graphs!
    for( AISNavigation::Graph::VertexIDMap::iterator it = optimizer->vertices().begin(); 
	    it != optimizer->vertices().end(); it++ )
    {
	// don't associate with self
	if( currentBodyFrame->getUniqueIdNumericalSuffix() != it->first )
	{
	    const int id = it->first; 
	    std::string env_item_id;
	    if(id == 0 || env->getEnvironmentPrefix() == "")
	    {
		env_item_id =  boost::lexical_cast<std::string>(id);
	    }
	    else
	    {
		env_item_id = env->getEnvironmentPrefix() + '/';
		env_item_id += boost::lexical_cast<std::string>(id);
	    }
	    envire::FrameNode::Ptr fn = env->getItem<envire::FrameNode>( env_item_id ).get();
	    std::cout << "associate node " << fn->getUniqueIdNumericalSuffix() << " with " << currentBodyFrame->getUniqueIdNumericalSuffix() << "... ";
	    bool result = associateNodes( fn.get(), currentBodyFrame.get() ); 
	    std::cout << (result ? "match" : "no match") << std::endl;
	}
    }

    // perform the graph optimization
    const int iterations = 5;
    optimizer->optimize( iterations, true );

    mlsGrid->clear();
    mlsOp->updateAll();

    // write the poses back to the environment
    // for now, write all the poses back, could add an updated flag
    for( AISNavigation::Graph::VertexIDMap::iterator it = optimizer->vertices().begin(); 
	    it != optimizer->vertices().end(); it++ )
    {
	const int id = it->first;
	const AISNavigation::PoseGraph3D::Vertex* vertex = static_cast<AISNavigation::PoseGraph3D::Vertex*>(it->second);

	// get pose with uncertainty from Hogman
	envire::TransformWithUncertainty tu( 
		hogman2Eigen( vertex->transformation ), 
		hogmanCov2EnvireCov( vertex->covariance ) );

	// and set it in envire for the frameNode with the corresponding id
	std::string env_item_id;
	if(id == 0 || env->getEnvironmentPrefix() == "")
	{
	    env_item_id =  boost::lexical_cast<std::string>(id);
	}
	else
	{
	    env_item_id = env->getEnvironmentPrefix() + '/';
	    env_item_id += boost::lexical_cast<std::string>(id);
	}
	envire::FrameNode::Ptr fn = env->getItem<envire::FrameNode>( env_item_id ).get();
	fn->setTransform( tu );

	// update the bounds 
	// TODO this could be optimized, as it may be to expensive to 
	// update the bounds everytime we have a small change in position
	getSensorMaps( fn.get() )->updateBounds();
    }

    // TODO see if we need to reassociate here 

    bodyFrame->setTransform( currentBodyFrame->getTransform() );
    prevBodyFrame = currentBodyFrame;
    currentBodyFrame = NULL;
}

/** will return a sensormaps structure for a given 
 * framenode. creates a new one, if not already existing.
 */
SensorMaps* PoseGraph::getSensorMaps( envire::FrameNode* fn )
{
    const long id = fn->getUniqueIdNumericalSuffix();

    // see if we can return a cached object
    std::map<long, SensorMaps*>::iterator 
	f = nodeMap.find( id );

    if( f != nodeMap.end() )
	return f->second;

    // otherwise create a new node
    SensorMaps* sm = new VisualSensorMaps( fn );
    nodeMap.insert( make_pair( id, sm ) );

    // call update bounds once, so we have an initial
    // idea of the bounds
    sm->updateBounds();

    return sm;
}


/** 
 * associate two framenodes, if they are within a feasable distance
 * between each other and have overlapping bounding boxes.
 *
 * @return true if an association has been added
 */ 
bool PoseGraph::associateNodes( envire::FrameNode* a, envire::FrameNode* b )
{
    // discard if distance between is too high
    // TODO: this is potentially dangerous as it doesn't take the
    // uncertainty into account... see how to make this safer, but still
    // fast.
    if( (a->getTransform().translation() - b->getTransform().translation()).norm() > max_node_radius )
	return false;

    // get the sensor map objects for both frameNodes
    SensorMaps 
	*sma = getSensorMaps( a ), 
	*smb = getSensorMaps( b );

    // check if the bounding boxes have a common intersection
    // and return false if not
    if( sma->bounds.intersection( smb->bounds ).isEmpty() )
	return false;

    std::vector<envire::TransformWithUncertainty> constraints;
    sma->associate( smb, constraints );
    for( size_t i = 0; i < constraints.size(); i++ )
    {
	// add the egde to the optimization framework 
	// this will update an existing edge
	optimizer->addEdge( 
		optimizer->vertex( a->getUniqueIdNumericalSuffix() ),
		optimizer->vertex( b->getUniqueIdNumericalSuffix() ),
		eigen2Hogman( constraints[i].getTransform() ),
		envireCov2HogmanInf( constraints[i].getCovariance() )
		);
    }

    // TODO store both successful and unsuccessful associations
    // since it doesn't make sense to try to associate twice
    return !constraints.empty();
}

}