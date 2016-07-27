/* Generated from orogen/lib/orogen/templates/tasks/Task.cpp */


#include "Task.hpp"
#include <visp/vpPixelMeterConversion.h>
#include <visp/vpFeatureBuilder.h>

using namespace visp;

Task::Task(std::string const& name)
    : TaskBase(name), tu(vpFeatureThetaU::cdRc), t(vpFeatureTranslation::cdMc)
{
}

Task::Task(std::string const& name, RTT::ExecutionEngine* engine)
    : TaskBase(name, engine), tu(vpFeatureThetaU::cdRc), t(vpFeatureTranslation::cdMc)
{
}

Task::~Task()
{
}

/// The following lines are template definitions for the various state machine
// hooks defined by Orocos::RTT. See Task.hpp for more detailed
// documentation about them.

bool Task::configureHook()
{
    if (! TaskBase::configureHook())
        return false;

    //Set the target_list
    target_list = _target_list.get();

    // Initialize camera parameters.
    // The library desconsider the tangencial values, that's why only the
    // radial values are informed.
    frame_helper::CameraCalibration cal = _camera_parameters.get();
    double scaling = _scaling.get();
    if(scaling != 1.0)
    {
        cal.fx = scaling * cal.fx;
        cal.fy = scaling * cal.fy;
        cal.cx = scaling * cal.cx;
        cal.cy = scaling * cal.cy;
        cal.height = scaling * cal.height;
        cal.width = scaling * cal.width;
    }
    cam_cal = vpCameraParameters(cal.fx, cal.fy,cal.cx,cal.cy);

    //Center of the target (reference point)
    P.setWorldCoordinates(0, 0, 0);

    // Define the task
    task.setServo(vpServo::EYEINHAND_L_cVe_eJe) ;
    task.setInteractionMatrixType(vpServo::CURRENT, vpServo::PSEUDO_INVERSE);

    switch(_architecture.get().mode)
    {
        case IBVS:
            task.addFeature(p,pd) ;
            break;
        case PBVS:
            task.addFeature(t);
            task.addFeature(tu);
            break;
        case HVS:
            task.addFeature(p,pd) ;
            task.addFeature(depth) ;
            task.addFeature(tu) ;
    }

    expected_inputs = _expected_inputs.get();
    // Set the Jacobian (expressed in the end-effector frame)
    // it depends of the desired control dof
    eJe = getJacobianFromExpectedInputs(expected_inputs);
    task.set_eJe(eJe);

    // Set the pose transformation between the camera and the robot frame
    Eigen::Affine3d body2camera;
    _body2camera.get(base::Time::now(), body2camera);

    //Computes the Velocity Twist Matrix from the spatial transformation between the camera and the robot
    //translation
    vpTranslationVector ctb(body2camera.translation()[0],
            body2camera.translation()[1],
            body2camera.translation()[2]);
    //rotation
    base::Quaterniond q(body2camera.rotation());
    vpRotationMatrix cRb(vpQuaternionVector(q.x(), q.y(), q.z(), q.w()));

    //build the homogeneous matrix of the body frame wrt the camera
    cMb.buildFrom(ctb, cRb);
    //build and set the velocity twist matrix
    cVb.buildFrom(cMb);
    task.set_cVe(cVb);

    return true;
}

bool Task::startHook()
{
    if (! TaskBase::startHook())
        return false;
    return true;
}

void Task::updateHook()
{
    TaskBase::updateHook();

    //read input ports
    std::vector<apriltags::VisualFeaturePoint> corners_vector;
    bool no_corners = (_marker_corners.read(corners_vector) != RTT::NewData);
    bool no_setpoint = (_cmd_in.read(setpoint) == RTT::NoData);

    //set the state CONTROLLING only if there is setpoint and detected corners
    if (no_corners)
    {
        state(WAITING_CORNERS);
    }
    else if (no_setpoint)
    {
        state(WAITING_SETPOINT);
    }
    else if (state() != CONTROLLING)
    {
        state(CONTROLLING);
    }

    if (!(state() == CONTROLLING))
    {
        vpColVector v(6,0);
        writeVelocities(v);
    }
    else
    {
        // filter the corners of an desired target
        apriltags::VisualFeaturePoint corners;
        filterTarget(corners_vector, target_identifier, corners);

        // transforms the setpoint of object in the body frame to the camera
        // Sets the desired position of the visual feature (reference point)
        updateDesiredPose(setpoint);

        //if no target is informed, change the state and return
        updateTargetParameters(target_identifier);
        updateFeaturesHVS(corners);

        // update eJe
        task.set_eJe(eJe);

        // Set the gain
        setGain();

        // Compute the visual servoing skew vector
        vpColVector v;
        task.set_cVe(cVb);
        v = task.computeControlLaw() ;

        // Compute the norm-2 of the error vector
        ctrl_state.error = vpMath::sqr((task.getError()).sumSquare());

        writeVelocities(v);
        _controller_state.write(ctrl_state);
    }
}

void Task::errorHook()
{
    TaskBase::errorHook();
}

void Task::stopHook()
{
    TaskBase::stopHook();
}

void Task::cleanupHook()
{
    TaskBase::cleanupHook();

    task.kill();
}

void Task::updateFeaturesHVS(apriltags::VisualFeaturePoint corners)
{
    //  Compute the initial pose
    pose.clearPoint();

    for (int i=0 ; i < 4 ; i++)
    {
        base::Vector2d aux;
        aux = corners.points[i];
        //vpImagePoint is instanciated using (v,u) instead of (u,v)
        vpImagePoint a(aux[1],aux[0]);
        double x,y;
        vpPixelMeterConversion::convertPoint(cam_cal,a,x,y);
        point[i].set_x(x);
        point[i].set_y(y);
        pose.addPoint(point[i]) ;
    }

    pose.computePose(vpPose::LAGRANGE, cMo);
    pose.computePose(vpPose::VIRTUAL_VS, cMo);

    // apply a rotation around the z on the camera frame
    vpHomogeneousMatrix rot(0, 0, 0, 0, 0, vpMath::rad(rotation_around_z));

    cMo = cMo * rot;
    //compute the "quality" of the result
    ctrl_state.residual = pose.computeResidual(cMo);

    //////////////////////////////
    // Update Feature 1 - p    //
    //////////////////////////////
    // Sets the current position of the visual feature
    P.track(cMo);

    ctrl_state.current_pose = convertToRbs(cMo);
    vpFeatureBuilder::create(p, P);

    //////////////////////////////
    // Update Feature 2 - depth //
    //////////////////////////////

    //Takes the features, calculate the intersection of the diagonals of the square and
    //given the camera parameters, retrieve the coordinates in meters.
    vpImagePoint refP;
    vpImagePoint cog[4];

    for (int i=0; i< 4; i++)
    {
        base::Vector2d aux;
        aux = corners.points[i];
        //according to vpImagePoint documentation (i,j) is equivalent to (v,u)
        cog[i].set_ij(aux[1], aux[0]);
    }

    // Compute the coordinates of the 2D reference point from the
    // intersection of the diagonals of the square
    double a1 = (cog[0].get_i()- cog[2].get_i()) /
        (cog[0].get_j()- cog[2].get_j());
    double a2 = (cog[1].get_i()- cog[3].get_i()) /
        (cog[1].get_j()- cog[3].get_j());
    double b1 = cog[0].get_i() - a1*cog[0].get_j();
    double b2 = cog[1].get_i() - a2*cog[1].get_j();

    refP.set_j( (b1-b2) / (a2-a1) );
    refP.set_i( a1*refP.get_j() + b1 );
    double x,y;
    vpPixelMeterConversion::convertPoint(cam_cal, refP, x, y);

    // Update the 2D ref point visual feature
    double Z;
    Z = P.get_Z();
    p.set_xyZ(x, y, Z);
    depth.buildFrom(P.get_x(), P.get_y(), Z, log(Z/Zd));

    //////////////////////////////
    // Update Feature 3 - t, tu //
    //////////////////////////////
    cdMc = cdMo*cMo.inverse();
    t.buildFrom(cdMc);
    tu.buildFrom(cdMc) ;

}

bool Task::updateDesiredPose(base::LinearAngular6DCommand setpoint)
{
    transformInput(setpoint);
    ctrl_state.desired_pose = convertToRbs(cdMo);

    P.track(cdMo);
    vpFeatureBuilder::create(pd, P);
    Zd = P.get_Z();

    return true;
}

void Task::setGain()
{
    if (_use_adaptive_gain.get())
    {
        vpAdaptiveGain  lambda;
        lambda.initStandard(_adaptive_gains.get().gain_at_zero,
                _adaptive_gains.get().gain_at_infinity,
                _adaptive_gains.get().slope_at_zero);

        task.setLambda(lambda) ;
    }
    else
    {
        task.setLambda(_gain.get());
    }
}

void Task::writeVelocities(vpColVector v)
{
    base::LinearAngular6DCommand vel;
    visp::saturationValues saturation = _saturation.get();

    for (int i=0; i < 3; ++i)
    {
        // write NaN on the non-desirable outputs in order to
        // keep the consistence with the other controllers
        // linear
        if (!expected_inputs.linear[i])
        {
            vel.linear[i] = base::NaN<double>();
        }
        else if (v[i] > saturation.linear[i])
        {
            vel.linear[i] = saturation.linear[i];
        }

        //angular
        if (!expected_inputs.angular[i])
        {
            vel.angular[i] = base::NaN<double>();
        }
        else if (v[i+3] > saturation.angular[i])
        {
            vel.angular[i] = saturation.angular[i];
        }
    }

    vel.time = ctrl_state.timestamp;
    _cmd_out.write(vel);
}

base::samples::RigidBodyState Task::convertToRbs(vpHomogeneousMatrix pose)
{
    base::samples::RigidBodyState rbs;

    rbs.position[0] = pose.getTranslationVector()[0];
    rbs.position[1] = pose.getTranslationVector()[1];
    rbs.position[2] = pose.getTranslationVector()[2];

    vpRotationMatrix R;
    pose.extract(R);
    vpQuaternionVector q(R);
    rbs.orientation = base::Orientation(q.w(), q.x(), q.y(), q.z());

    return rbs;
}

void Task::transformInput(base::LinearAngular6DCommand cmd_in_body)
{
    //initialize setpoint on the body frame
    //since the non-expected input are NaN, set it to default values
    //in order to have valid values for the HomogeneusMatrix creation
    for (int i = 0; i < 3; ++i)
    {
        if (!expected_inputs.linear[i]) {cmd_in_body.linear[i] = 0;}

        // the object frame's z-axis points to the oposite side of the body 
        // frame's x-axis. The object x and y axis are in such way that in order 
        // to have a vehicle stable in "d" meters in front of the object, the desired
        // posistion has to be: (x=d, y=0, z=0, roll=-pi/2, pitch=-pi/2, yaw=0).
        // So if no roll and pitch control is desired, the default value has to
        // be -M_PI_2 for these DOF.
        if (!expected_inputs.angular[i])
        {
            if ( i == 0 || i == 1)
            {
                cmd_in_body.angular[i] = -M_PI_2;
            }
            else
            {
                cmd_in_body.angular[i] = 0;
            }
        }
    }

    //create a vpHomogeneousMatrix of the object desired position wrt body (bdMo)
    vpTranslationVector bdto(cmd_in_body.linear[0], cmd_in_body.linear[1], cmd_in_body.linear[2]);
    vpRxyzVector bdro(cmd_in_body.angular[0], cmd_in_body.angular[1], cmd_in_body.angular[2]);
    vpRotationMatrix bdRo(bdro);
    //pose of the object wrt the body
    vpHomogeneousMatrix bdMo(bdto, bdRo);

    //bdMo is the desired pose of the object in the body frame
    //cdMo is the desired pose of the object in the camera frame
    //cMb is the transformation matrix of the body in the camera frame
    cdMo = cMb * bdMo;
}

vpMatrix Task::getJacobianFromExpectedInputs(visp::expectedInputs expected_inputs)
{
    vpMatrix eJe;
    eJe.eye(6);

    for(int i = 0; i < 3; ++i)
    {
        if (!expected_inputs.linear[i])
            eJe[i][i] = 0;
        if (!expected_inputs.angular[i])
            eJe[i+3][i+3] = 0;
    }

    return eJe;
}

bool Task::filterTarget(std::vector<apriltags::VisualFeaturePoint> corners_vector, std::string target_identifier, apriltags::VisualFeaturePoint &corners)
{
    if (corners_vector.size() == 0)
    {
        return false;
    }
    else if (target_identifier == "")
    {
        ctrl_state.timestamp = corners_vector[0].time;
        corners = corners_vector[0];
        return true;
    }
    else
    {
        for (int i = 0; i < corners_vector.size(); ++i)
        {
            if (target_identifier == corners_vector[i].identifier)
            {
                ctrl_state.timestamp = corners_vector[i].time;
                corners = corners_vector[i];
                return true;
            }
        }
        return false;
    }
}

bool Task::updateTargetParameters(std::string target_identifier)
{
    if (target_identifier == "")
    {
        //update object size
        setObjectSize(_object_width.get(), _object_height.get());
        //update rotation
        rotation_around_z = _rotation_around_z.get();
        return true;
    }
    else
    {
        for (int i = 0; i < target_list.size(); ++i)
        {
            if(target_list[i].identifier == target_identifier)
            {
                //update the size of the object
                setObjectSize(target_list[i].width, target_list[i].height);
                //update the rotation
                rotation_around_z = target_list[i].rotation_around_z;

                //set the properties
                _object_height.set(target_list[i].width);
                _object_width.set(target_list[i].height);
                _rotation_around_z.set(target_list[i].rotation_around_z);
                return true;
            }
        }
        return false;
    }
}

void Task::setObjectSize(double object_width, double object_height)
{
    double w = object_width/2;
    double h = object_height/2;

    point[0].setWorldCoordinates(-w,  h, 0);
    point[1].setWorldCoordinates( w,  h, 0);
    point[2].setWorldCoordinates( w, -h, 0);
    point[3].setWorldCoordinates(-w, -h, 0);
}

bool Task::setTarget_identifier(std::string const & value)
{
    this->target_identifier = value;
    return true;
}
