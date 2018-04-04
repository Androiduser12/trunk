

#include "Lubrication.hpp"

YADE_PLUGIN((Ip2_FrictMat_FrictMat_LubricationPhys)(LubricationPhys)(Law2_ScGeom_ImplicitLubricationPhys))


CREATE_LOGGER(LubricationPhys);

void Ip2_FrictMat_FrictMat_LubricationPhys::go(const shared_ptr<Material> &material1, const shared_ptr<Material> &material2, const shared_ptr<Interaction> &interaction)
{
    if (interaction->phys)
        return;

    // Cast to Lubrication
    shared_ptr<LubricationPhys> phys(new LubricationPhys());
    FrictMat* mat1 = YADE_CAST<FrictMat*>(material1.get());
    FrictMat* mat2 = YADE_CAST<FrictMat*>(material2.get());

    // Copy from HertzMindlin
    /* from interaction physics */
    Real Ea = mat1->young;
    Real Eb = mat2->young;
    Real Va = mat1->poisson;
    Real Vb = mat2->poisson;
    Real fa = mat1->frictionAngle;
    Real fb = mat2->frictionAngle;


    /* from interaction geometry */
    GenericSpheresContact* scg = YADE_CAST<GenericSpheresContact*>(interaction->geom.get());
    Real Da = scg->refR1>0 ? scg->refR1 : scg->refR2;
    Real Db = scg->refR2;
    //Vector3r normal=scg->normal;        //The variable set but not used


    /* calculate stiffness coefficients */
    Real Ga = Ea/(2*(1+Va));
    Real Gb = Eb/(2*(1+Vb));
    Real G = (Ga+Gb)/2; // average of shear modulus
    Real V = (Va+Vb)/2; // average of poisson's ratio
    Real E = Ea*Eb/((1.-std::pow(Va,2))*Eb+(1.-std::pow(Vb,2))*Ea); // Young modulus
    Real R = Da*Db/(Da+Db); // equivalent radius
    Real a = (Da+Db)/2.;
    Real Kno = 4./3.*E*sqrt(R); // coefficient for normal stiffness
    Real Kso = 2*sqrt(4*R)*G/(2-V); // coefficient for shear stiffness
    Real frictionAngle = std::min(fa,fb);
    
    phys->kno = Kno;
    phys->kso = Kso;
    phys->mum = std::tan(frictionAngle);
    phys->nun = M_PI*eta*3./2.*a*a;
    phys->u = -1;
    phys->prevDotU = 0;

    phys->eta = eta;
    phys->eps = eps;
    interaction->phys = phys;
}
CREATE_LOGGER(Ip2_FrictMat_FrictMat_LubricationPhys);


Real Law2_ScGeom_ImplicitLubricationPhys::normalForce_trapezoidalScheme(Real const& un, LubricationPhys* phys, ScGeom* geom, Scene* scene)
{
    Real a((geom->radius1+geom->radius2)/2.);
	
	Real u = trpz_integrate_u(	phys->prevDotU, phys->prev_un  /*prev. un*/,
					un, phys->u, phys->nun, phys->kn, phys->kn /*should be keps, currently both are equal*/, 
					phys->eps*a, scene->dt, phys->u<phys->eps*a, theta == 1. /*firstOrder*/);
	
	phys->prev_un = un;
	phys->contact = u < phys->eps*a;
	phys->normalForce = phys->kn*(un-u)*geom->normal;
	phys->normalContactForce = ((phys->contact) ? Vector3r(phys->kn*(u-phys->eps*a)*geom->normal) : Vector3r::Zero());
	phys->normalLubricationForce = phys->normalForce - phys->normalContactForce;
			
	return u;
}

Real Law2_ScGeom_ImplicitLubricationPhys::trpz_integrate_u(Real& prevDotU, const Real& un_prev, Real un_curr, const Real& u_prev,
						      const Real& nu, Real k, const Real& keps, const Real& eps, 
						      Real dt, bool withContact, bool firstOrder)
{
	Real u=0;// gap distance (by which normal lubrication terms are divided)
	bool hadContact = u_prev<eps;
	Real initPrevDotU = prevDotU; //backup since it may be necessary to go back to it when switching with/without contact
	Real /*a=1*/b,c;
	Real w=nu/(dt*k);
	
	// if contact through roughness is assumed it implies modified coefficients in the ODE compared to no-contact solution.
	// Changes of status are checked at the end
	if (withContact)
	{
		k = k + keps;
		un_curr = (k*un_curr+keps*eps)/(k+keps);
	}
	
	// polynomial a*u²+b*u+c=0 with a=1, integrating du/dt=k*u*(un-u)/nu
// 	if (!firstOrder) {b=2*w-un_curr; c=-prevDotU-2.*w*u_prev; /*implicit trapezoidal rule 2nd order*/ } 
	if (!firstOrder)
	{
		b=w/theta-un_curr;
		c=(-prevDotU*(1.-theta)-w*u_prev)/theta; /*implicit theta method*/
	} 
	else
	{
		b= nu/dt/k-un_curr;
		c = -w*u_prev; /*implicit backward Euler 1st order*/
	}
	
	Real delta = b*b-4.*c;//note: a=1
	if (delta<0.) LOG_WARN("Negative delta should not happen, needs a fix (please report)");
	Real rr[2]={0.5*(-b+sqrt(delta)),0.5*(-b-sqrt(delta))};//roots	
// 	cerr<<"1 "<<b<<" "<<c<<" | "<<prevDotU <<" "<< un_prev <<" "<< un_curr<<" "<< u_prev<<" "<< nu<<" "<< k<<" "<< keps<<" "<<dt <<" "<<rr[0]<<" "<<rr[1]<<" "<<(rr[0]>0?rr[0]:rr[1])-un_curr <<endl;
	
	if (delta<0. or (rr[0]<=0. and rr[1]<=0.))
	{// recursive calls after halving the time increment if no positive solution found
		LOG_WARN("delta<0 or roots <= 0, sub-stepping with dt="<<dt/2.);
		Real un_mid = un_prev+0.5*(un_curr-un_prev);

		Real u_mid = trpz_integrate_u(prevDotU, un_prev,un_mid,u_prev,nu,k,keps,eps,dt/2.,withContact,firstOrder);
		return trpz_integrate_u(prevDotU, un_mid,un_curr,u_mid,nu,k,keps,eps,dt/2.,withContact,firstOrder);
	}
	else
	{	// normal case, keep the positive solution closest to the previous one
		if ((std::abs(rr[0]-u_prev)<std::abs(rr[1]-u_prev) and rr[0]>0.) or rr[1]<0.)
			u = rr[0];
		else
			u = rr[1];
		
		//LOG_DEBUG("rr0 " << rr[0] << " rr1 " << rr[1] << " u " << u); 
		
		bool hasContact = u<eps;
		if (withContact and not hasContact)
		{
			if (debug) LOG_WARN("withContact and not hasContact");
			prevDotU = initPrevDotU;//re-init for the different scenario
			return trpz_integrate_u(prevDotU, un_prev,un_curr,u_prev,nu,k,keps,eps,dt,false,firstOrder);
		}
		else if (not withContact and hasContact)
		{
			if(debug) LOG_WARN("withContact=false and hasContact");
			prevDotU = initPrevDotU;//re-init for the different scenario
			return trpz_integrate_u(prevDotU, un_prev,un_curr,u_prev,nu,k,keps,eps,dt,true,firstOrder);
		}
		const Real& weight=trpzWeight;
		prevDotU = weight*u*(un_curr-u)+(1.-weight)*prevDotU;//set for next iteration
		return u;
	}
}

void Law2_ScGeom_ImplicitLubricationPhys::shearForce_firsorderScheme(Real const& u, LubricationPhys* phys, ScGeom *geom, Scene* scene)
{
    Vector3r Ft(Vector3r::Zero());
    Vector3r Ft_ = geom->rotate(phys->shearForce);
    const Vector3r& dus = geom->shearIncrement();
	phys->slip = false;
	const Real& kt = phys->ks;
	const Real& nut = phys->cs;
	
	// Also work without fluid (nut == 0)
	if(phys->contact) // This come from normal part
	{
		Ft = Ft_ + kt*dus; // Trial force
		phys->shearContactForce = Ft; // If no slip: no lubrication!
#if 1
		if(Ft.norm() > phys->normalContactForce.norm()*std::max(0.,phys->mum)) // If slip
		{
			//LOG_INFO("SLIP");
			Ft *= phys->normalContactForce.norm()*std::max(0.,phys->mum)/Ft.norm();
			phys->shearContactForce = Ft;
			Ft = (Ft*kt*scene->dt + Ft_*nut + dus*kt*nut)/(kt*scene->dt+nut);
			phys->slip = true;
			phys->shearLubricationForce = nut*dus/scene->dt;
		}
#endif
	}
	else
	{
		Ft = (Ft_ + dus*kt)*nut/(nut+kt*scene->dt);
		phys->shearLubricationForce = Ft;
	}
	
	phys->shearForce = Ft;
}

bool Law2_ScGeom_ImplicitLubricationPhys::go(shared_ptr<IGeom> &iGeom, shared_ptr<IPhys> &iPhys, Interaction *interaction)
{

    // Physic
    LubricationPhys* phys=static_cast<LubricationPhys*>(iPhys.get());

    // Geometry
    ScGeom* geom=static_cast<ScGeom*>(iGeom.get());

    // Get bodies properties
    Body::id_t id1 = interaction->getId1();
    Body::id_t id2 = interaction->getId2();
    const shared_ptr<Body> b1 = Body::byId(id1,scene);
    const shared_ptr<Body> b2 = Body::byId(id2,scene);
    State* s1 = b1->state.get();
    State* s2 = b2->state.get();

    // geometric parameters
    Real a((geom->radius1+geom->radius2)/2.);
    Real un(-geom->penetrationDepth);

    // Speeds
    Vector3r shiftVel=scene->isPeriodic ? Vector3r(scene->cell->velGrad*scene->cell->hSize*interaction->cellDist.cast<Real>()) : Vector3r::Zero();
    Vector3r shift2 = scene->isPeriodic ? Vector3r(scene->cell->hSize*interaction->cellDist.cast<Real>()): Vector3r::Zero();

    Vector3r relV = geom->getIncidentVel(s1, s2, scene->dt, shift2, shiftVel, false );
//    Vector3r relVN = relV.dot(norm)*norm; // Normal velocity
//    Vector3r relVT = relV - relVN; // Tangeancial velocity
    Real undot = relV.dot(geom->normal); // Normal velocity norm
    
    if(un > a)
    {
        //FIXME: it needs to go to potential always based on distance, the "undot < 0" here is dangerous (let the collider do its job)
        return undot < 0; // Only go to potential if distance is increasing 
    }

    // Normal part : init
    Real u = un; // u is interface distance
    
    if(phys->u == -1)
	{
		phys->u = u;
	}
    
    Real delt = std::max(phys->ue, a/100.);
	
    phys->kn = phys->kno*std::pow(delt,0.5);
	phys->normalForce = Vector3r::Zero();
    phys->normalContactForce = Vector3r::Zero();
    phys->normalLubricationForce = Vector3r::Zero();
    
    if(activateNormalLubrication)
    {
        if (resolutionMethod == 0)
        {
			// Functions are responsible for setting forces and contact in physics. (And everything else they want)
	        u = normalForce_trapezoidalScheme(un,phys,geom,scene);
		}
		
		phys->u = u;
		phys->ue = u - un;
    }
    
    // Init shear and torques
	phys->shearForce = Vector3r::Zero();
	phys->shearLubricationForce = Vector3r::Zero();
	phys->shearContactForce = Vector3r::Zero();
	Vector3r Cr = Vector3r::Zero();
	Vector3r Ct = Vector3r::Zero();
	
    if((phys->eta > 0. && u > 0.) || phys->eta <= 0.)
	{
		// Tangencial force
		phys->ks = phys->kso*std::pow(delt,0.5);
		phys->cs = (phys->eta > 0.) ? M_PI*phys->eta/2.*(-2.*a+(2.*a+u)*std::log((2.*a+u)/u)) : 0.;
		phys->cn = phys->nun/u;
		
		if(activateTangencialLubrication)
		{
			// This function is responsible for setting shears forces (and everything else they want)
			shearForce_firsorderScheme(u,phys,geom,scene);
		}

		// Rolling and twist torques
		Vector3r relAngularVelocity = geom->getRelAngVel(s1,s2,scene->dt);
		Vector3r relTwistVelocity = relAngularVelocity.dot(geom->normal)*geom->normal;
		Vector3r relRollVelocity = relAngularVelocity - relTwistVelocity;

		if(activateRollLubrication && phys->eta > 0.) Cr = M_PI*phys->eta*a*a*a*(3./2.*std::log(a/u)+63./500.*u/a*std::log(a/u))*relRollVelocity;
		if (activateTwistLubrication && phys->eta > 0.) Ct = M_PI*phys->eta*a*un*std::log(a/u)*relTwistVelocity;
	}
	else
		LOG_ERROR("u<=0 with lubrication. This should not happen: please report. (shear force and torques calculation disabled for this step) u=" << u);

    // total torque
    Vector3r C1 = -(geom->radius1+un/2.)*phys->shearForce.cross(geom->normal)+Cr+Ct;
    Vector3r C2 = -(geom->radius2+un/2.)*phys->shearForce.cross(geom->normal)-Cr-Ct;

    // Apply to bodies
    scene->forces.addForce(id1,phys->normalForce+phys->shearForce);
    scene->forces.addTorque(id1,C1);

    scene->forces.addForce(id2,-phys->normalForce-phys->shearForce);
    scene->forces.addTorque(id2,C2);

    return true;
}

CREATE_LOGGER(Law2_ScGeom_ImplicitLubricationPhys);

void Law2_ScGeom_ImplicitLubricationPhys::getStressForEachBody(vector<Matrix3r>& NCStresses, vector<Matrix3r>& SCStresses, vector<Matrix3r>& NLStresses, vector<Matrix3r>& SLStresses)
{
  	const shared_ptr<Scene>& scene=Omega::instance().getScene();
	NCStresses.resize(scene->bodies->size());
	SCStresses.resize(scene->bodies->size());
	NLStresses.resize(scene->bodies->size());
	SLStresses.resize(scene->bodies->size());
	for (size_t k=0;k<scene->bodies->size();k++)
	{
		NCStresses[k]=Matrix3r::Zero();
		SCStresses[k]=Matrix3r::Zero();
		NLStresses[k]=Matrix3r::Zero();
		SLStresses[k]=Matrix3r::Zero();
	}
	FOREACH(const shared_ptr<Interaction>& I, *scene->interactions){
		if(!I->isReal()) continue;
		GenericSpheresContact* geom=YADE_CAST<GenericSpheresContact*>(I->geom.get());
		LubricationPhys* phys=YADE_CAST<LubricationPhys*>(I->phys.get());
		
		if(phys)
		{
			Vector3r lV1 = (3.0/(4.0*Mathr::PI*pow(geom->refR1,3)))*((geom->contactPoint-Body::byId(I->getId1(),scene)->state->pos));
			Vector3r lV2 = Vector3r::Zero();
			if (!scene->isPeriodic)
				lV2 = (3.0/(4.0*Mathr::PI*pow(geom->refR2,3)))*((geom->contactPoint- (Body::byId(I->getId2(),scene)->state->pos)));
			else
				lV2 = (3.0/(4.0*Mathr::PI*pow(geom->refR2,3)))*((geom->contactPoint- (Body::byId(I->getId2(),scene)->state->pos + (scene->cell->hSize*I->cellDist.cast<Real>()))));

			NCStresses[I->getId1()] += phys->normalContactForce*lV1.transpose();
			NCStresses[I->getId2()] -= phys->normalContactForce*lV2.transpose();
			SCStresses[I->getId1()] += phys->shearContactForce*lV1.transpose();
			SCStresses[I->getId2()] -= phys->shearContactForce*lV2.transpose();
			NLStresses[I->getId1()] += phys->normalLubricationForce*lV1.transpose();
			NLStresses[I->getId2()] -= phys->normalLubricationForce*lV2.transpose();
			SLStresses[I->getId1()] += phys->shearLubricationForce*lV1.transpose();
			SLStresses[I->getId2()] -= phys->shearLubricationForce*lV2.transpose();
		}
	}
}

py::tuple Law2_ScGeom_ImplicitLubricationPhys::PyGetStressForEachBody()
{
	py::list nc, sc, nl, sl;
	vector<Matrix3r> NCs, SCs, NLs, SLs;
	getStressForEachBody(NCs, SCs, NLs, SLs);
	FOREACH(const Matrix3r& m, NCs) nc.append(m);
	FOREACH(const Matrix3r& m, SCs) sc.append(m);
	FOREACH(const Matrix3r& m, NLs) nl.append(m);
	FOREACH(const Matrix3r& m, SLs) sl.append(m);
	return py::make_tuple(nc, sc, nl, sl);
}

void Law2_ScGeom_ImplicitLubricationPhys::getTotalStresses(Matrix3r& NCStresses, Matrix3r& SCStresses, Matrix3r& NLStresses, Matrix3r& SLStresses)
{
	vector<Matrix3r> NCs, SCs, NLs, SLs;
	getStressForEachBody(NCs, SCs, NLs, SLs);
    
  	const shared_ptr<Scene>& scene=Omega::instance().getScene();
    
    if(!scene->isPeriodic)
    {
        LOG_ERROR("This method can only be used in periodic simulations");
        return;
    }
    
    for(unsigned int i(0);i<NCs.size();i++)
    {
        Sphere * s = YADE_CAST<Sphere*>(Body::byId(i,scene)->shape.get());
        
        if(s)
        {
            Real vol = 4./3.*M_PI*pow(s->radius,3);
            
            NCStresses += NCs[i]*vol;
            SCStresses += SCs[i]*vol;
            NLStresses += NLs[i]*vol;
            SLStresses += SLs[i]*vol;
        }
    }
    
    NCStresses /= scene->cell->getVolume();
    SCStresses /= scene->cell->getVolume();
    NLStresses /= scene->cell->getVolume();
    SLStresses /= scene->cell->getVolume();
}

py::tuple Law2_ScGeom_ImplicitLubricationPhys::PyGetTotalStresses()
{
	Matrix3r nc(Matrix3r::Zero()), sc(Matrix3r::Zero()), nl(Matrix3r::Zero()), sl(Matrix3r::Zero());

    getTotalStresses(nc, sc, nl, sl);
	return py::make_tuple(nc, sc, nl, sl);
}


