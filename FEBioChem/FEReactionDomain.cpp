#include "stdafx.h"
#include "FEReactionDomain.h"
#include <FECore/FEModel.h>
#include <FECore/Integrate.h>

FEDomain* FEReactionDomainFactory::CreateDomain(const FE_Element_Spec& spec, FEMesh* pm, FEMaterial* pmat)
{
	FE_Element_Class eclass = spec.eclass;
	if (dynamic_cast<FEReactionDiffusionMaterial*>(pmat))
	{
		if (eclass == FE_ELEM_SOLID)
		{
			FEReactionDomain* dom = new FEReactionDomain(pmat->GetFEModel());
			dom->SetMaterial(pmat);
			return dom;
		}
	}
	return 0;
}


FEReactionDomain::FEReactionDomain(FEModel* fem) : FESolidDomain(fem)
{
	m_mat = 0;
}

//-----------------------------------------------------------------------------
// Assigns material to domain
void FEReactionDomain::SetMaterial(FEMaterial* pmat)
{
	m_mat = dynamic_cast<FEReactionDiffusionMaterial*>(pmat);
}

//-----------------------------------------------------------------------------
// Initializes domain data.
// This creates a list of active degrees of freedom in this domain
bool FEReactionDomain::Initialize()
{
	// do base class first
	if (FESolidDomain::Initialize() == false) return false;

	// make sure we have a material
	if (m_mat == 0) return false;

	// get all the concentration dofs
	DOFS& dofs = m_mat->GetFEModel()->GetDOFS();
	vector<int> c;
	dofs.GetDOFList("concentration", c);

	// get all the concentration degrees of freedom that are active in this domain
	int ns = m_mat->Species();
	vector<int> dofList(ns);
	for (int i = 0; i<ns; ++i)
	{
		FEReactiveSpecies* s = m_mat->GetSpecies(i);
		dofList[i] = c[s->GetID()];
	}
	SetDOF(dofList);

	return true;
}

//-----------------------------------------------------------------------------
void FEReactionDomain::Update(const FETimeInfo& tp)
{
	const vector<int>& dofs = GetDOFList();
	int ndof = (int)dofs.size();

	FEMesh& mesh = *GetMesh();

	int nspecies = m_mat->Species();
	assert(nspecies == ndof);

	int NE = Elements();
	for (int iel = 0; iel<NE; ++iel)
	{
		// get the next element 
		FESolidElement& el = Element(iel);

		// get the current nodal concentration values
		int ne = el.Nodes();
		vector<vector<double> > c(ndof, vector<double>(ne));
		for (int i = 0; i<ne; ++i)
		{
			FENode& node = mesh.Node(el.m_node[i]);
			for (int j = 0; j<ndof; ++j) c[j][i] = node.get(dofs[j]);
		}

		// evaluate integration point values
		int nint = el.GaussPoints();
		for (int n = 0; n<nint; ++n)
		{
			FEMaterialPoint& mp = *el.GetMaterialPoint(n);
			FEReactionMaterialPoint& rp = *mp.ExtractData<FEReactionMaterialPoint>();

			// evaluate concentrations at integration points
			for (int i = 0; i<nspecies; ++i)
			{
				FEReactiveSpecies* s = m_mat->GetSpecies(i);
				rp.m_c[s->GetID()] = el.Evaluate(&(c[i][0]), n);
			}
		}
	}
}

//-----------------------------------------------------------------------------
void FEReactionDomain::ForceVector(FEGlobalVector& R)
{
	// get the number of degrees of freedom active in this domain
	int ndof = GetDOFS();

	vector<double> fe;
	vector<int> lm;

	int NE = Elements();
	for (int i = 0; i<NE; ++i)
	{
		FESolidElement& el = Element(i);

		int neln = el.Nodes();
		fe.resize(ndof*neln);
		zero(fe);

		// evaluate the element force vector
		ElementForceVector(el, fe);

		// get the LM array
		UnpackLM(el, lm);

		// assemble into global array
		R.Assemble(lm, fe);
	}
}

//-----------------------------------------------------------------------------
void FEReactionDomain::ElementForceVector(FESolidElement& el, vector<double>& fe)
{
	const vector<int>& dofs = GetDOFList();
	int ndof = (int) dofs.size();
	vector<double> R(ndof, 0.0);

	// loop over all integration points
	int ne = el.Nodes();
	int ni = el.GaussPoints();
	double* gw = el.GaussWeights();
	for (int n=0; n<ni; ++n)
	{
		FEMaterialPoint& mp = *el.GetMaterialPoint(n);
		FEReactionMaterialPoint& pt = *mp.ExtractData<FEReactionMaterialPoint>();

		// evaluate the reaction rates
		for (int i=0; i<ndof; ++i)
		{
			R[i] = m_mat->GetReactionRate(pt, m_mat->GetSpecies(i)->GetID());
		}

		double detJ = detJt(el, n);
		double* H = el.H(n);
		for (int i=0; i<ne; ++i)
		{
			for (int j=0; j<ndof; ++j)
			{
				fe[i*ndof + j] += H[i]*R[j]*gw[n]*detJ;
			}
		}
	}
}

//-----------------------------------------------------------------------------
void FEReactionDomain::StiffnessMatrix(FELinearSystem& K, const FETimeInfo& ti)
{
	// add "mass" matrix
	MassMatrix(K, ti.timeIncrement);

	// add diffusion stiffness
	DiffusionMatrix(K, ti);
}

//-----------------------------------------------------------------------------
void FEReactionDomain::MassMatrix(FELinearSystem& K, double dt)
{
	// get the number of concentration variables
	const vector<int>& dofs = GetDOFList();
	int ncv = (int)dofs.size();

	vector<int> lm;
	matrix me;

	FEMesh& mesh = *GetMesh();

	int NE = Elements();
	for (int iel=0; iel<NE; ++iel)
	{
		FESolidElement& el = Element(iel);
		int ne = el.Nodes();
		int ndof = ne*ncv;

		// initialize the element mass matrix
		me.resize(ndof, ndof);
		me.zero();

		// evaluate element mass matrix
		ElementMassMatrix(el, me);

		// get the lm array
		UnpackLM(el, lm);

		// this component needs to be assembled to the LHS and RHS
		K.AssembleLHS(lm, me);

		// get the nodal values
		vector<double> fe(ndof, 0.0);
		for (int i=0; i<ne; ++i)
		{
			for (int j = 0; j<ncv; ++j)
			{
				double cn = mesh.Node(el.m_node[i]).get(dofs[j]);
				fe[i*ncv + j] = cn;
			}
		}

		// multiply with me
		fe = me*fe;

		// assemble this vector to the right-hand side
		K.AssembleRHS(lm, fe);
	}
}

//-----------------------------------------------------------------------------
void FEReactionDomain::ElementMassMatrix(FESolidElement& el, matrix& ke)
{
	int ncv = GetDOFS();
	int ne = el.Nodes();
	int nint = el.GaussPoints();
	double* gw = el.GaussWeights();
	for (int n=0; n<nint; ++n)
	{
		// element shape function values at integration point n
		double* H = el.H(n);

		// Jacobian at this point
		double detJ = detJt(el, n);

		for (int a=0; a<ne; ++a)
		{
			for (int b=0; b<ne; ++b)
			{
				double kab = H[a]*H[b]*gw[n]*detJ;

				for (int i=0; i<ncv; ++i) ke[a*ncv + i][b*ncv + i] += kab;
			}
		}
	}
}

//-----------------------------------------------------------------------------
void FEReactionDomain::DiffusionMatrix(FELinearSystem& K, const FETimeInfo& ti)
{
	// get the number of concentration variables
	const vector<int>& dofs = GetDOFList();
	int ncv = (int)dofs.size();

	vector<int> lm;
	matrix ke;

	FEMesh& mesh = *GetMesh();

	int NE = Elements();
	for (int iel = 0; iel<NE; ++iel)
	{
		FESolidElement& el = Element(iel);
		int ne = el.Nodes();
		int ndof = ne*ncv;

		// get the lm array
		UnpackLM(el, lm);

		// initialize the element diffusion matrix
		ke.resize(ndof, ndof);
		ke.zero();

		// evaluate element mass matrix
		ElementDiffusionMatrix(el, ke);

		if (ti.alpha > 0.0)
		{
			ke *= ti.alpha*ti.timeIncrement;

			// this component needs to be assembled to the LHS and RHS
			K.AssembleLHS(lm, ke);
		}

		if (ti.alpha < 1.0)
		{
			// get the nodal values
			vector<double> fe(ndof, 0.0);
			for (int i = 0; i<ne; ++i)
			{
				for (int j = 0; j<ncv; ++j)
				{
					double cn = mesh.Node(el.m_node[i]).get(dofs[j]);
					fe[i*ncv + j] = cn;
				}
			}

			if (ti.alpha > 0.0)
				ke *= -(1.0 - ti.alpha)/ti.alpha;
			else
				ke *= ti.timeIncrement;

			fe = ke*fe;

			K.AssembleRHS(lm, fe);
		}
	}
}

//-----------------------------------------------------------------------------
void FEReactionDomain::ElementDiffusionMatrix(FESolidElement& el, matrix& ke)
{
	// get the number of concentration variables
	const vector<int>& dofs = GetDOFList();
	int ncv = (int)dofs.size();

	// get the diffusion coefficients
	vector<double> D(ncv);
	for (int i=0; i<ncv; ++i) D[i] = m_mat->GetSpecies(i)->Diffusivity();

	int ne = el.Nodes();
	int ni = el.GaussPoints();

	const int EN = FEElement::MAX_NODES;
	double Gx[EN], Gy[EN], Gz[EN];
	double Ji[3][3];
	double Gi[3], Gj[3];
	double DB[3];

	// loop over all integration points
	const double *gw = el.GaussWeights();
	for (int n = 0; n<ni; ++n)
	{
		// calculate jacobian
		double detJt = invjact(el, Ji, n);

		// evaluate the conductivity
		FEMaterialPoint& mp = *el.GetMaterialPoint(n);

		for (int i = 0; i<ne; ++i)
		{
			double Gr = el.Gr(n)[i];
			double Gs = el.Gs(n)[i];
			double Gt = el.Gt(n)[i];

			// calculate global gradient of shape functions
			// note that we need the transposed of Ji, not Ji itself !
			Gx[i] = Ji[0][0] * Gr + Ji[1][0] * Gs + Ji[2][0] * Gt;
			Gy[i] = Ji[0][1] * Gr + Ji[1][1] * Gs + Ji[2][1] * Gt;
			Gz[i] = Ji[0][2] * Gr + Ji[1][2] * Gs + Ji[2][2] * Gt;
		}

		for (int a = 0; a<ne; ++a)
		{
			Gi[0] = Gx[a];
			Gi[1] = Gy[a];
			Gi[2] = Gz[a];

			for (int b = 0; b<ne; ++b)
			{
				Gj[0] = Gx[b];
				Gj[1] = Gy[b];
				Gj[2] = Gz[b];

				for (int i = 0; i<ncv; ++i)
				{
					DB[0] = D[i]*Gj[0];
					DB[1] = D[i]*Gj[1];
					DB[2] = D[i]*Gj[2];

					double kab = (Gi[0] * DB[0] + Gi[1] * DB[1] + Gi[2] * DB[2])*detJt*gw[n];

					ke[a*ncv + i][b*ncv + i] += kab;
				}
			}
		}
	}
}

//-----------------------------------------------------------------------------
void FEReactionDomain::ElementReactionStiffness(FESolidElement& el, matrix& ke)
{
	// get the number of concentration variables
	const vector<int>& dofs = GetDOFList();
	int ncv = (int)dofs.size();

	int ne = el.Nodes();
	double Ji[3][3];

	matrix Gamma(ncv, ncv);
	Gamma.zero();

	// loop over all integration points
	int ni = el.GaussPoints();
	const double *gw = el.GaussWeights();
	for (int n = 0; n<ni; ++n)
	{
		// element shape function values at integration point n
		double* H = el.H(n);

		// calculate jacobian
		double detJt = invjact(el, Ji, n);

		// get the material point data
		FEMaterialPoint& mp = *el.GetMaterialPoint(n);
		FEReactionMaterialPoint& rp = *mp.ExtractData<FEReactionMaterialPoint>();

		// evaluate the gamma matrix
		for (int i=0; i<ncv; ++i)
			for (int j=0; j<ncv; ++j)
				{
					Gamma[i][j] = m_mat->GetReactionRateStiffness(rp, m_mat->GetSpecies(i)->GetID(), m_mat->GetSpecies(j)->GetID());
				}

		// evaluate element matrix
		for (int a = 0; a<ne; ++a)
		{
			for (int b = 0; b<ne; ++b)
			{
				for (int i=0; i<ncv; ++i)
				{
					for (int j=0; j<ncv; ++j)
					{
						double gij = Gamma[i][j];

						double kpq = gij*H[a]*H[b]*gw[n]*detJt;

						// NOTE: The negative sign is because we need to subtract this matrix from the global matrix
						ke[a*ncv + i][b*ncv + j] -= kpq;
					}
				}	
			}
		}
	}
}

void FEReactionDomain::ElementConvectionMatrix(FESolidElement& el, matrix& ke, const vector<vec3d>& vn)
{
	// get the number of concentration variables
	const vector<int>& dofs = GetDOFList();
	int ncv = (int)dofs.size();

	int ne = el.Nodes();
	int ni = el.GaussPoints();

	const int EN = FEElement::MAX_NODES;
	double Gx[EN], Gy[EN], Gz[EN];
	double Ji[3][3];
	double Gj[3];

	// loop over all integration points
	const double *gw = el.GaussWeights();
	for (int n = 0; n<ni; ++n)
	{
		// calculate jacobian
		double detJt = invjact(el, Ji, n);

		// shape functions
		double* H = el.H(n);

		vec3d vi = el.Evaluate((vec3d*)(&vn[0]), n);

		// evaluate shape function derivatives
		for (int i = 0; i<ne; ++i)
		{
			double Gr = el.Gr(n)[i];
			double Gs = el.Gs(n)[i];
			double Gt = el.Gt(n)[i];

			// calculate global gradient of shape functions
			// note that we need the transposed of Ji, not Ji itself !
			Gx[i] = Ji[0][0] * Gr + Ji[1][0] * Gs + Ji[2][0] * Gt;
			Gy[i] = Ji[0][1] * Gr + Ji[1][1] * Gs + Ji[2][1] * Gt;
			Gz[i] = Ji[0][2] * Gr + Ji[1][2] * Gs + Ji[2][2] * Gt;
		}

		// loop over all nodes
		for (int a = 0; a<ne; ++a)
		{
			for (int b = 0; b<ne; ++b)
			{
				Gj[0] = Gx[b];
				Gj[1] = Gy[b];
				Gj[2] = Gz[b];

				double kab = (H[a] * (vi.x*Gj[0] + vi.y*Gj[1] + vi.z*Gj[2]))*detJt*gw[n];

				for (int i = 0; i<ncv; ++i) ke[a*ncv + i][b*ncv + i] += kab;
			}
		}
	}
}
