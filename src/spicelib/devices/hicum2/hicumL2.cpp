/**********
Copyright 1990 Regents of the University of California.  All rights reserved.
Author: 1985 Thomas L. Quarles
Model Author: 1990 Michael Schröter TU Dresden
Spice3 Implementation: 2019 Dietmar Warning, Markus Müller, Mario Krattenmacher
**********/

/*
 * This file defines the HICUM L2.4.0 model load function
 * Comments on the Code:
 * - We use dual numbers to calculate derivatives, this is readble and error proof.
 * - The code is targeted to be readbale and maintainable, speed is sacrificied for this purpose.
 * - The verilog a code is available at the website of TU Dresden, Michael Schroeter#s chair.
 *
 * Checklist of what needs to be done: (@Mario: also look at this, did I get everything?)
 * - ijBEp
 * - ijBCx
 * - QjEp
 * - QBCx'
 * - QBCx''
 * - QdS
 * - QjS
 * - iTS
 * - ijSC
 * - rbi
 * - crbi,qrbi
 * - Qjci
 * - Qjei
 * - ijBCi
 * - ijBEi
 * - Qf
 * - Qr
 * - iavl
 * - iBEti
 * - itf, itr
 * - NQS derivatives, sources and elements
 */

#include "cmath"
#include <duals/dual>
#include "hicumL2.hpp"
#include <functional>

//ngspice header files written in C
#ifdef __cplusplus
extern "C"
{
#endif
#include "ngspice/typedefs.h"
#include "ngspice/devdefs.h"
#include "ngspice/const.h"
#include "ngspice/trandefs.h"
#include "ngspice/sperror.h"
#include "hicum2defs.h"
#include "ngspice/ngspice.h"
#include "ngspice/cktdefs.h"
#ifdef __cplusplus
}
#endif

// extern "C"
// {
// #include "ngspice/devdefs.h"
// #include "ngspice/const.h"
// #include "ngspice/trandefs.h"
// #include "ngspice/sperror.h"
// #include "hicum2defs.h"
// #include "ngspice/ngspice.h"
// #include "ngspice/cktdefs.h"
// }


// #include "ngspice/devdefs.h"
// #include "ngspice/const.h"
// #include "ngspice/trandefs.h"
// #include "ngspice/sperror.h"
// #include "hicum2defs.h"
// #include "ngspice/ngspice.h"
// #include "ngspice/cktdefs.h"


//HICUM DEFINITIONS
#define CHARGE          1.6021766208e-19
#define CONSTboltz      1.38064852e-23
#define VPT_thresh      1.0e2
#define Dexp_lim        80.0
#define Cexp_lim        80.0
#define DFa_fj          1.921812
#define RTOLC           1.0e-5
#define l_itmax         100
#define TMAX            326.85
#define TMIN            -100.0
#define LN_EXP_LIMIT    11.0
#define MIN_R           0.001
#define Gmin            1.0e-12


using namespace duals::literals;

// IDEAL DIODE (WITHOUT CAPACITANCE):
// conductance calculation not required
// INPUT:
//  IS, IST     : saturation currents (model parameter related)
//  UM1         : ideality factor
//  U           : branch voltage
// IMPLICIT INPUT:
//  T           : Temperature
// OUTPUT:
//  Iz          : diode current
duals::duald HICDIO(duals::duald T, double IST, double UM1, duals::duald U)
{
duals::duald DIOY, le, vt;

    vt = CONSTboltz * T / CHARGE;
    DIOY = U/(UM1*vt);
    // le = exp(DIOY); // would be the best way... But stay close to HICUML2.va
    // return IST*(le-1.0);
    if (IST > 0.0) {
        if (DIOY > Dexp_lim) {
            le      = (1 + (DIOY - Dexp_lim));
            DIOY    = Dexp_lim;
            return IST*(le*exp(DIOY)-1.0);
        } else if (DIOY <= -14.0) {
            return -IST;
        } else {
            le      = exp(DIOY);
            return IST*(le-1.0);
        }
    } else {
        return 0.0;
    }
}

// DEPLETION CHARGE CALCULATION
// Hyperbolic smoothing used; no punch-through
// INPUT:
//  c_0		: zero-bias capacitance
//  u_d		: built-in voltage
//  z		: exponent coefficient
//  a_j		: control parameter for C peak value at high forward bias
//  U_cap	: voltage across junction
// IMPLICIT INPUT:
//  T		: Temperature
// OUTPUT:
//  Qz		: depletion Charge
//  C		: depletion capacitance
void QJMODF(duals::duald T, double c_0, double u_d, double z, double a_j, duals::duald U_cap, duals::duald * C, duals::duald * Qz)
{
    duals::duald DFV_f, DFv_e, DFs_q, DFs_q2, DFv_j, DFdvj_dv, DFQ_j, DFQ_j1, DFC_j1, DFb, vt;
    vt = CONSTboltz * T / CHARGE;
    if (c_0 > 0.0) {
        DFV_f	 = u_d*(1.0-exp(-log(a_j)/z));
        DFv_e	 = (DFV_f-U_cap)/vt;
        DFs_q	 = sqrt(DFv_e*DFv_e+DFa_fj);
        DFs_q2	 = (DFv_e+DFs_q)*0.5;
        DFv_j	 = DFV_f-vt*DFs_q2;
        DFdvj_dv = DFs_q2/DFs_q;
        DFb	     = log(1.0-DFv_j/u_d);
        DFC_j1   = c_0*exp(-z*DFb)*DFdvj_dv;
        *C		 = DFC_j1+a_j*c_0*(1.0-DFdvj_dv);
        DFQ_j	 = c_0*u_d*(1.0-exp(DFb*(1.0-z)))/(1.0-z);
        *Qz	     = DFQ_j+a_j*c_0*(U_cap-DFv_j);
     } else {
        *C       = 0.0;
        *Qz	     = 0.0;
     }
}

// DEPLETION CHARGE CALCULATION CONSIDERING PUNCH THROUGH
// smoothing of reverse bias region (punch-through)
// and limiting to a_j=Cj,max/Cj0 for forward bias.
// Important for base-collector and collector-substrate junction
// INPUT:
//  c_0		: zero-bias capacitance
//  u_d		: built-in voltage
//  z 		: exponent coefficient
//  a_j		: control parameter for C peak value at high forward bias
//  v_pt	: punch-through voltage (defined as qNw^2/2e)
//  U_cap	: voltage across junction
// IMPLICIT INPUT:
//  VT		: thermal voltage
// OUTPUT:
//  Qz		: depletion charge
//  C		: depletion capacitance
void QJMOD(duals::duald T, duals::duald c_0, double u_d, double z, double a_j, double v_pt, duals::duald U_cap, duals::duald * C, duals::duald * Qz)
{
    duals::duald dummy, DQ_j1, DQ_j2, DQ_j3, DC_j1, DC_j2, DC_j3, De_1, De_2, Dzr1, DCln1, DCln2, Dz1, Dv_j1, Dv_j2, Dv_j3, De, Da, Dv_r, Dv_j4, Dv_e, DC_c, DC_max, DV_f, Dv_p, Dz_r, vt;
    vt = CONSTboltz * T / CHARGE;
    if (c_0 > 0.0){
        Dz_r	= z/4.0;
        Dv_p	= v_pt-u_d;
        DV_f	= u_d*(1.0-exp(-log(a_j)/z));
        DC_max	= a_j*c_0;
        DC_c	= c_0*exp((Dz_r-z)*log(v_pt/u_d));
        Dv_e	= (DV_f-U_cap)/vt;
        if(Dv_e < Cexp_lim) {
            De	    = exp(Dv_e);
            De_1	= De/(1.0+De);
            Dv_j1	= DV_f-vt*log(1.0+De);
        } else {
            De_1	= 1.0;
            Dv_j1	= U_cap;
        }
        Da	    = 0.1*Dv_p+4.0*vt;
        Dv_r	= (Dv_p+Dv_j1)/Da;
        if(Dv_r < Cexp_lim){
            De	    = exp(Dv_r);
            De_2	= De/(1.0+De);
            Dv_j2	= -Dv_p+Da*(log(1.0+De)-exp(-(Dv_p+DV_f)/Da));
        } else {
            De_2	= 1.0;
            Dv_j2	= Dv_j1;
        }
        Dv_j4	= U_cap-Dv_j1;
        DCln1	= log(1.0-Dv_j1/u_d);
        DCln2	= log(1.0-Dv_j2/u_d);
        Dz1	    = 1.0-z;
        Dzr1	= 1.0-Dz_r;
        DC_j1	= c_0*exp(DCln2*(-z))*De_1*De_2;
        DC_j2	= DC_c*exp(DCln1*(-Dz_r))*(1.0-De_2);
        DC_j3	= DC_max*(1.0-De_1);
        *C		= DC_j1+DC_j2+DC_j3;
        DQ_j1	= c_0*(1.0-exp(DCln2*Dz1))/Dz1;
        DQ_j2	= DC_c*(1.0-exp(DCln1*Dzr1))/Dzr1;
        DQ_j3	= DC_c*(1.0-exp(DCln2*Dzr1))/Dzr1;
        *Qz	    = (DQ_j1+DQ_j2-DQ_j3)*u_d+DC_max*Dv_j4;
     } else {
        *C	    = 0.0;
        *Qz	    = 0.0;
     }
}

// A CALCULATION NEEDED FOR COLLECTOR MINORITY CHARGE FORMULATION
// INPUT:
//  zb,zl       : zeta_b and zeta_l (model parameters, TED 10/96)
//  w           : normalized injection width
// OUTPUT:
// hicfcio      : function of equation (2.1.17-10)
void HICFCI(double zb, double zl, double w, double * hicfcio, double * dhicfcio_dw)
{
    double a, a2, a3, r, lnzb, x, z;
    z       = zb*w;
    lnzb    = log(1+zb*w);
    if(z > 1.0e-6){
        x               = 1.0+z;
        a               = x*x;
        a2              = 0.250*(a*(2.0*lnzb-1.0)+1.0);
        a3              = (a*x*(3.0*lnzb-1.0)+1.0)/9.0;
        r               = zl/zb;
        *hicfcio        = ((1.0-r)*a2+r*a3)/zb;
        *dhicfcio_dw    = ((1.0-r)*x+r*a)*lnzb;
     } else {
        a               = z*z;
        a2              = 3.0+z-0.25*a+0.10*z*a;
        a3              = 2.0*z+0.75*a-0.20*a*z;
        *hicfcio         = (zb*a2+zl*a3)*w*w/6.0;
        *dhicfcio_dw     = (1+zl*w)*(1+z)*lnzb;
     }
}

// NEEDED TO CALCULATE WEIGHTED ICCR COLLECTOR MINORITY CHARGE
// INPUT:
//  z : zeta_b or zeta_l
//  w : normalized injection width
// OUTPUT:
//  hicfcto     : output
//  dhicfcto_dw : derivative of output wrt w
void HICFCT(double z, double w, double * hicfcto, double *dhicfcto_dw)
{
    double a, lnz;
    a = z*w;
    lnz = log(1+z*w);
    if (a > 1.0e-6){
        *hicfcto     = (a - lnz)/z;
        *dhicfcto_dw = a / (1.0 + a);
    } else {
        *hicfcto     = 0.5 * a * w;
        *dhicfcto_dw = a;
    }
}

// COLLECTOR CURRENT SPREADING CALCULATION
// collector minority charge incl. 2D/3D current spreading (TED 10/96)
// INPUT:
//  Ix                          : forward transport current component (itf)
//  I_CK                        : critical current
//  FFT_pcS                     : dependent on fthc and thcs (parameters)
// IMPLICIT INPUT:
//  ahc, latl, latb             : model parameters
//  VT                          : thermal voltage
// OUTPUT:
//  Q_fC, Q_CT: actual and ICCR (weighted) hole charge
//  T_fC, T_cT: actual and ICCR (weighted) transit time
//  Derivative dfCT_ditf not properly implemented yet
void HICQFC(duals::duald T, double Ix, double I_CK, double FFT_pcS, duals::duald * Q_fC, duals::duald * Q_CT, duals::duald * T_fC, duals::duald * T_cT)
{
    double FCln, FCa, FCa1, FCd_a, FCw, FCdw_daick, FCda1_dw, FCf_ci, FCdfCT_ditf, FCw2, FCz, FCdfc_dw, FFdVc_ditf, FCf_CT, FCf1, FCf2, FCrt;
    double FCa_cl, FCa_ck, FCdaick_ditf, FCxl, FCxb, FCdf1_dw, FCz_1, FCf3, FCdf2_dw, FCdf3_dw, FCdw_ditf, FCdfc_ditf;
    double FCdfCT_dw, FCd_f, FFdVc;
    double vcbar, latl, latb, ahc, flcomp;

    duals::duald vt;

    vt = CONSTboltz * T / CHARGE;

    *Q_fC           = FFT_pcS*Ix;
    FCa             = 1.0-I_CK/Ix;
    FCrt            = sqrt(FCa*FCa+ahc);
    FCa_ck          = 1.0-(FCa+FCrt)/(1.0+sqrt(1.0+ahc));
    FCdaick_ditf    = (FCa_ck-1.0)*(1-FCa)/(FCrt*Ix);
    if(latb > latl){
        FCz             = latb-latl;
        FCxl            = 1.0+latl;
        FCxb            = 1.0+latb;
        if(latb > 0.01){
            FCln            = log(FCxb/FCxl);
            FCa1            = exp((FCa_ck-1.0)*FCln);
            FCd_a           = 1.0/(latl-FCa1*latb);
            FCw             = (FCa1-1.0)*FCd_a;
            FCdw_daick      = -FCz*FCa1*FCln*FCd_a*FCd_a;
            FCa1            = log((1.0+latb*FCw)/(1.0+latl*FCw));
            FCda1_dw        = latb/(1.0+latb*FCw) - latl/(1.0+latl*FCw);
        } else {
            FCf1            = 1.0-FCa_ck;
            FCd_a           = 1.0/(1.0+FCa_ck*latb);
            FCw             = FCf1*FCd_a;
            FCdw_daick      = -1.0*FCd_a*FCd_a*FCxb*FCd_a;
            FCa1            = FCz*FCw;
            FCda1_dw        = FCz;
        }
        FCf_CT          = 2.0/FCz;
        FCw2            = FCw*FCw;
        FCf1            = latb*latl*FCw*FCw2/3.0+(latb+latl)*FCw2/2.0+FCw;
        FCdf1_dw        = latb*latl*FCw2 + (latb+latl)*FCw + 1.0;
        HICFCI(latb,latl,FCw,&FCf2,&FCdf2_dw);
        HICFCI(latl,latb,FCw,&FCf3,&FCdf3_dw);
        FCf_ci          = FCf_CT*(FCa1*FCf1-FCf2+FCf3);
        FCdfc_dw        = FCf_CT*(FCa1*FCdf1_dw+FCda1_dw*FCf1-FCdf2_dw+FCdf3_dw);
        FCdw_ditf       = FCdw_daick*FCdaick_ditf;
        FCdfc_ditf      = FCdfc_dw*FCdw_ditf;
        if(flcomp == 0.0 || flcomp == 2.1) {
            HICFCT(latb,FCw,&FCf2,&FCdf2_dw);
            HICFCT(latl,FCw,&FCf3,&FCdf3_dw);
            FCf_CT          = FCf_CT*(FCf2-FCf3);
            FCdfCT_dw       = FCf_CT*(FCdf2_dw-FCdf3_dw);
            FCdfCT_ditf     = FCdfCT_dw*FCdw_ditf;
        } else {
            FCf_CT          = FCf_ci;
            FCdfCT_ditf     = FCdfc_ditf;
        }
     } else {
        if(latb > 0.01) {
            FCd_a           = 1.0/(1.0+FCa_ck*latb);
            FCw             = (1.0-FCa_ck)*FCd_a;
            FCdw_daick      = -(1.0+latb)*FCd_a*FCd_a;
        } else {
            FCw             = 1.0-FCa_ck-FCa_ck*latb;
            FCdw_daick      = -(1.0+latb);
        }
        FCw2            = FCw*FCw;
        FCz             = latb*FCw;
        FCz_1           = 1.0+FCz;
        FCd_f           = 1.0/(FCz_1);
        FCf_ci          = FCw2*(1.0+FCz/3.0)*FCd_f;
        FCdfc_dw        = 2.0*FCw*(FCz_1+FCz*FCz/3.0)*FCd_f*FCd_f;
        FCdw_ditf       = FCdw_daick*FCdaick_ditf;
        FCdfc_ditf      = FCdfc_dw*FCdw_ditf;
        if(flcomp == 0.0 || flcomp == 2.1){
            if (FCz > 0.001){
                FCf_CT          = 2.0*(FCz_1*log(FCz_1)-FCz)/(latb*latb*FCz_1);
                FCdfCT_dw       = 2.0*FCw*FCd_f*FCd_f;
            } else {
                FCf_CT          = FCw2*(1.0-FCz/3.0)*FCd_f;
                FCdfCT_dw       = 2.0*FCw*(1.0-FCz*FCz/3.0)*FCd_f*FCd_f;
            }
            FCdfCT_ditf     = FCdfCT_dw*FCdw_ditf;
        } else {
            FCf_CT          = FCf_ci;
            FCdfCT_ditf     = FCdfc_ditf;
        }
    }
    *Q_CT    = *Q_fC*FCf_CT*exp((FFdVc-vcbar)/vt);
    *Q_fC    = *Q_fC*FCf_ci*exp((FFdVc-vcbar)/vt);
    *T_fC    = FFT_pcS*exp((FFdVc-vcbar)/vt)*(FCf_ci+Ix*FCdfc_ditf) +*Q_fC/vt*FFdVc_ditf;
    *T_cT    = FFT_pcS*exp((FFdVc-vcbar)/vt)*(FCf_CT+Ix*FCdfCT_ditf)+*Q_CT/vt*FFdVc_ditf;
}

// DEPLETION CHARGE & CAPACITANCE CALCULATION SELECTOR
// Dependent on junction punch-through voltage
// Important for collector related junctions
void HICJQ(duals::duald T, double c_0, double u_d, double z,double v_pt, duals::duald U_cap, duals::duald * C,duals::duald * Qz)
{
    if(v_pt < VPT_thresh){
        QJMOD(T,c_0,u_d,z,2.4,v_pt,U_cap,C,Qz);
    } else {
        QJMODF(T,c_0,u_d,z,2.4,U_cap,C,Qz);
    }
}

// TRANSIT-TIME AND STORED MINORITY CHARGE
// INPUT:
//  itf         : forward transport current
//  I_CK        : critical current
//  T_f         : transit time    \
//  Q_f         : minority charge / for low current
// IMPLICIT INPUT:
//  tef0, gtfe, fthc, thcs, ahc, latl, latb     : model parameters
// OUTPUT:
//  T_f         : transit time    \
//  Q_f         : minority charge / transient analysis
//  T_fT        : transit time    \
//  Q_fT        : minority charge / ICCR (transfer current)
//  Q_bf        : excess base charge
void HICQFF(duals::duald T, double itf, double I_CK, duals::duald T_f, duals::duald Q_f, duals::duald T_fT, duals::duald Q_fT, duals::duald Q_bf)
{
    double FFitf_ick, FFdTef, FFdQef, FFdVc, FFdVc_ditf, FFib, FFfcbar, FFdib_ditf;
    double icbar, hfc_t, hfe_t, hf0_t, vlim, rci0, gtfe, latl, latb, vcbar, fthc, acbar, tef0_t, ahc, thcs_t;
    duals::duald vt;
    duals::duald FFdQbfb, FFdTbfb, FFdQfhc, FFdTfhc, FFdQcfc,FFdTcfc, FFdQbfc,FFdTbfc;
    duals::duald FFdQcfcT, FFic, FFw, FFdTcfcT;
    vt = CONSTboltz * T / CHARGE;
    if(itf < 1.0e-6*I_CK){
        Q_fT            = Q_f;
        T_fT            = T_f;
        Q_bf            = 0;
    } else {
        FFitf_ick = itf/I_CK;
        FFdTef  = tef0_t*exp(gtfe*log(FFitf_ick));
        FFdQef  = FFdTef*itf/(1+gtfe);
        if (icbar<0.05*(vlim/rci0)) {
            FFdVc = 0;
            FFdVc_ditf = 0;
        } else {
            FFib    = (itf-I_CK)/icbar;
            if (FFib < -1.0e10) {
                FFib = -1.0e10;
            }
            FFfcbar = (FFib+sqrt(FFib*FFib+acbar))/2.0;
            FFdib_ditf = FFfcbar/sqrt(FFib*FFib+acbar)/icbar;
            FFdVc = vcbar*exp(-1.0/FFfcbar);
            FFdVc_ditf = FFdVc/(FFfcbar*FFfcbar)*FFdib_ditf;
        }
        FFdQbfb = (1-fthc)*thcs_t*itf*(exp(FFdVc/vt)-1);
        FFdTbfb = FFdQbfb/itf+(1-fthc)*thcs_t*itf*exp(FFdVc/vt)/vt*FFdVc_ditf;
        FFic    = 1-1.0/FFitf_ick;
        FFw     = (FFic+sqrt(FFic*FFic+ahc))/(1+sqrt(1+ahc));
        FFdQfhc = thcs_t*itf*FFw*FFw*exp((FFdVc-vcbar)/vt);
        FFdTfhc = FFdQfhc*(1.0/itf*(1.0+2.0/(FFitf_ick*sqrt(FFic*FFic+ahc)))+1.0/vt*FFdVc_ditf);
        if(latb <= 0.0 && latl <= 0.0){
            FFdQcfc = fthc*FFdQfhc;
            FFdTcfc = fthc*FFdTfhc;
            FFdQcfcT = FFdQcfc;
            FFdTcfcT = FFdTcfc;
        } else {
            HICQFC(T, itf,I_CK,fthc*thcs_t,&FFdQcfc,&FFdQcfcT,&FFdTcfc,&FFdTcfcT);
        }
        FFdQbfc = (1-fthc)*FFdQfhc;
        FFdTbfc = (1-fthc)*FFdTfhc;
        Q_fT	= hf0_t*Q_f+FFdQbfb+FFdQbfc+hfe_t*FFdQef+hfc_t*FFdQcfcT;
        T_fT	= hf0_t*T_f+FFdTbfb+FFdTbfc+hfe_t*FFdTef+hfc_t*FFdTcfcT;
        Q_f	    = Q_f+(FFdQbfb+FFdQbfc)+FFdQef+FFdQcfc;
        T_f 	= T_f+(FFdTbfb+FFdTbfc)+FFdTef+FFdTcfc;
        Q_bf    = FFdQbfb+FFdQbfc;
    }
}

// TEMPERATURE UPDATE OF JUNCTION CAPACITANCE RELATED PARAMETERS
// INPUT:
//  mostly model parameters
//  x           : zero bias junction capacitance
//  y           : junction built-in potential
//  z           : grading co-efficient
//  w           : ratio of maximum to zero-bias value of capacitance or punch-through voltage
//  is_al       : condition factor to check what "w" stands for
//  vgeff       : band-gap voltage
// IMPLICIT INPUT:
//  VT          : thermal voltage
//  vt0,qtt0,ln_qtt0,mg : other model variables
// OUTPUT:
//  c_j_t               : temperature update of "c_j"
//  vd_t                : temperature update of "vd0"
//  w_t                 : temperature update of "w"
void TMPHICJ(duals::duald T, double c_j, double vd0, double z, double w, double is_al, double vgeff, duals::duald * c_j_t, duals::duald * vd_t, duals::duald * w_t)
{
    double vdj0, vt0;
    double mg, tnom;
    duals::duald vt, qtt0, ln_qtt0, vdt, vdjt;

    tnom    = tnom+300; //TODO: check this
    vt0     = CONSTboltz * tnom/ CHARGE;
    vt      = CONSTboltz * T   / CHARGE;
    qtt0    = T/tnom;
    ln_qtt0 = log(qtt0);

    //TODO
    //vt0,qtt0,lnqtt0,mg =
    if (c_j > 0.0) {
        vdj0    = 2*vt0*log(exp(vd0*0.5/vt0)-exp(-0.5*vd0/vt0));
        vdjt    = vdj0*qtt0+vgeff*(1-qtt0)-mg*vt*ln_qtt0;
        vdt     = vdjt+2*vt*log(0.5*(1+sqrt(1+4*exp(-vdjt/vt))));
        *vd_t    = vdt;
        *c_j_t   = c_j*exp(z*log(vd0/(*vd_t)));
        if (is_al == 1) {
            *w_t = w*(*vd_t)/vd0;
        } else {
            *w_t = w;
        }
    } else {
        *c_j_t   = c_j;
        *vd_t    = vd0;
        *w_t     = w;
    }
}

duals::duald calc_hjei_vbe(duals::duald Vbiei, duals::duald T, HICUMinstance * here, HICUMmodel * model){
    //calculates hje_vbe
    //warpping in a routine allows easy calculation of derivatives with dual numbers
    duals::duald vj, vj_z, vt;
    vt  = CONSTboltz * T   / CHARGE;
    if (model->HICUMahjei == 0.0){
        return model->HICUMhjei;
    }else{
        //vendhjei = vdei_t*(1.0-exp(-ln(ajei_t)/z_h));
        vj = (here->HICUMvdei_t-Vbiei)/(model->HICUMrhjei*vt);
        vj = here->HICUMvdei_t-model->HICUMrhjei*vt*(vj+sqrt(vj*vj+DFa_fj))*0.5;
        vj = (vj-vt)/vt;
        vj = vt*(1.0+(vj+sqrt(vj*vj+DFa_fj))*0.5);
        vj_z = (1.0-exp(model->HICUMzei*log(1.0-vj/here->HICUMvdei_t)))*here->HICUMahjei_t;
        return here->HICUMhjei0_t*(exp(vj_z)-1.0)/vj_z;
    }
}


void hicum_diode(double T, double IS, double UM1, double U, double *Iz, double *Gz, double *Tz)
{
    //wrapper for hicum diode equation that also generates derivatives
    duals::duald result = 0;

    // printf("executed diode");

    result = HICDIO(T, IS, UM1, U+1_e);
    *Iz    = result.rpart();
    *Gz    = result.dpart(); //derivative for U
    result = HICDIO(T+1_e, IS, UM1, U);
    *Tz    = result.dpart(); //derivative for T
}

void hicum_qjmodf(double T, double c_0, double u_d, double z, double a_j, double U_cap, double *C, double *C_dU, double *C_dT, double *Qz, double *Qz_dU, double *Qz_dT)
{
    //wrapper for QJMODF that also generates derivatives
    duals::duald Cresult = 0;
    duals::duald Qresult = 0;
    QJMODF(T, c_0, u_d, z, a_j, U_cap+1_e, &Cresult, &Qresult);
    *C     = Cresult.rpart();
    *C_dU  = Cresult.dpart();
    *Qz    = Qresult.rpart();
    *Qz_dU = Qresult.dpart();

    QJMODF(T+1_e, c_0, u_d, z, a_j, U_cap, &Cresult, &Qresult);
    *Qz_dT = Qresult.dpart();
    *C_dT  = Cresult.dpart();
}

void hicum_HICJQ(double T, double c_0, double u_d, double z,double v_pt, double U_cap, double * C, double * C_dU, double * C_dT, double * Qz, double * Qz_dU, double * Qz_dT)
{
    //wrapper for HICJQ that also generates derivatives
    duals::duald Cresult = 0;
    duals::duald Qresult = 0;
    HICJQ(T, c_0, u_d, z, v_pt, U_cap+1_e, &Cresult, &Qresult);
    *C     = Cresult.rpart();
    *C_dU  = Cresult.dpart();
    *Qz    = Qresult.rpart();
    *Qz_dU = Qresult.dpart();

    HICJQ(T+1_e, c_0, u_d, z, v_pt, U_cap+1_e, &Cresult, &Qresult);
    *Qz_dT = Qresult.dpart();
    *C_dT  = Cresult.dpart();
}

int
HICUMload(GENmodel *inModel, CKTcircuit *ckt)
        /* actually load the current resistance value into the
         * sparse matrix previously provided
         */
{
    HICUMmodel *model = (HICUMmodel*)inModel;
    HICUMinstance *here;

    //Declaration of variables

    double cbcpar1,cbcpar2,cbepar2,cbepar1,Oich,Otbhrec;

    //Charges, capacitances and currents
    double Qjci,Qjei,Qjep;
    double Qdei,Qdci,Qrbi;
    double it,ibei,irei,ibci,ibep,irep,ibh_rec;
    double ibet,iavl,iavl_ditf,iavl_dT,iavl_Vbiei,iavl_dCjci;
    double ijbcx,ijbcx_dT,ijbcx_Vbpci,ijsc,Qjs,Qscp,HSUM,HSI_Tsu,Qdsu;

    //Base resistance and self-heating power
    double rbi,pterm;

    //Model initialization
    double C_1;

    //Model evaluation
    double Crbi,Cjci,Cjcit,cc,Cjei,Cjep,Cjs,Cscp;
    double Cjcx_i , Cjcx_i_Vbci  , Cjcx_i_dT ;
    double Cjcx_ii, Cjcx_ii_Vbpci, Cjcx_ii_dT;
    double Qjcx_i , Qjcx_i_Vbci  , Qjcx_i_dT ;
    double Qjcx_ii, Qjcx_ii_Vbpci, Qjcx_ii_dT;

    double itf,itr,Tf,Tr,VT_f,i_0f,i_0r,a_bpt,Q_0,Q_p,Q_bpt;
    double Orci0_t,b_q,I_Tf1,T_f0,Q_fT,T_fT,Q_bf;
    double a_h,Q_pT,d_Q;
    double Qf, Qf_Vbiei, Qf_Vbici, Qf_dT, Cdei, Qr, Cdci;
    double ick, ick_Vciei, ick_dT,vc,cjcx01,cjcx02;
    int l_it;

    //NQS
    double Ixf1,Ixf2,Qxf1,Qxf2;
    double Itxf, Qdeix;
    double Vxf, Ixf, Qxf;
    double Vxf1, Vxf2;

    double hjei_vbe;

    double Vbiei, Vbici, Vciei, Vbpei, Vbpbi, Vbpci, Vsici, Vbci, Vsc;

    // Model flags
    int use_aval;

    //helpers for ngspice implementation
    duals::duald result;

    //end of variables

    int iret;

#ifndef PREDICTOR
    double xfact;
#endif
    double delvbiei=0.0, delvbici=0.0, delvbpei=0.0, delvbpbi=0.0, delvbpci=0.0, delvsici=0.0;
    double ibieihat;
    double ibpeihat;
    double icieihat;
    double ibicihat;
    double ibpcihat;
    double ibpbihat;
    double isicihat;
    double ibpsihat;
    double ceq, geq=0.0, rhs_current;
    int icheck=1;
    int ichk1, ichk2, ichk3, ichk4, ichk5;
    int error;
    double Vbe, Vcic, Vbbp, Veie, Vsis, Vbpe;

    double Ibiei, Ibiei_Vbiei;
    double Ibici, Ibici_Vbici;
    double Ibpei, Ibpei_Vbpei;
    double Ibpci, Ibpci_Vbpci;
    double Isici, Isici_Vsici;
    double Isc,   Isc_Vsc;
    double Iciei, Iciei_Vbiei, Iciei_Vbici;
    double Ibbp_Vbbp;
    double Isis_Vsis;
    double Ieie, Ieie_Veie;
    double Ibpbi, Ibpbi_Vbpbi, Ibpbi_Vbici, Ibpbi_Vbiei;
    double Ibpsi, Ibpsi_Vbpci, Ibpsi_Vsici;
    double Icic_Vcic;
    double ibci_Vbci, ibci_dT;
    double hjei_vbe_Vbiei, hjei_vbe_dT, ibet_Vbpei=0.0, ibet_dT=0, ibet_Vbiei=0.0, ibh_rec_Vbiei;
    double irei_Vbiei, irei_dT;
    double ibep_Vbpei, ibep_dT;
    double irep_Vbpei, irep_dT, iavl_Vbici, rbi_dT, rbi_dQjei, rbi_dCjci, rbi_dQf, rbi_Vbiei, rbi_Vbici;
    double ibei_Vbiei, ibei_dT;
    double Q_0_Vbiei, Q_0_Vbici, Q_0_hjei_vbe, Q_0_Qjci, Q_0_Qjei, Q_0_dT;

    double Cjei_Vbiei,Cjci_Vbici,Cjep_Vbpei,Cjep_dT,Cjs_Vsici,Cscp_Vsc,Cjcit_Vbici,i_0f_Vbiei,i_0r_Vbici;
    double Cjei_dT, Cjci_dT;
    double Qjei_Vbiei, Qjei_dT, Qjci_Vbici, Qjci_dT;
    double cc_Vbici,T_f0_Vbici,T_f0_Qjci, T_f0_dT,Q_p_Vbiei,Q_p_Vbici,I_Tf1_Vbiei,I_Tf1_Vbici,itf_Vbiei,itf_Vbici,itf_dT,itr_Vbiei,itr_Vbici;
    double Qbepar1;
    double Qbepar2;
    double Qbcpar1;
    double Qbcpar2;
    double Qsu;
    double Qcth;

    double Qrbi_Vbpbi;
    double Qrbi_Vbiei;
    double Qrbi_Vbici;
    double Qdeix_Vbiei;
    double Qdci_Vbici;
    double Qjep_Vbpei,Qjep_dT;
    double qjcx0_t_i_Vbci;
    double qjcx0_t_ii_Vbpci;
    double Qdsu_Vbpci;
    double Qjs_Vsici;
    double Qbepar1_Vbe;
    double Qbepar2_Vbpe;
    double Qbcpar1_Vbci;
    double Qbcpar2_Vbpci;
    double Qsu_Vsis;

    double cqbepar1, gqbepar1;
    double cqbepar2, gqbepar2;
    double cqbcpar1, gqbcpar1;
    double cqbcpar2, gqbcpar2;
    double cqsu, gqsu;
    double qjcx0_t_i, qjcx0_t_ii;

//NQS
    double Vbxf, Vbxf1, Vbxf2;
    double Qxf_Vxf, Qxf1_Vxf1, Qxf2_Vxf2;
    double Iqxf, Iqxf_Vxf, Iqxf1, Iqxf1_Vxf1, Iqxf2, Iqxf2_Vxf2;

    double Ith, Vrth, Icth, Icth_Vrth, delvrth;

    double Irth_Vrth;
    double Iciei_Vrth;
    double Ibiei_dT;
    double Ibici_Vrth;
    double Ibpei_Vrth;
    double Ibpci_Vrth;
    double Isici_Vrth;
    double Ibpbi_Vrth;
    double Ieie_Vrth;
    double Icic_Vrth;
    double Ibbp_Vrth;

    double Ith_Vrth;
    double Ith_Vciei;
    double Ith_Vbiei;
    double Ith_Vbici;
    double Ith_Vbpei;
    double Ith_Vbpci;
    double Ith_Vsici;
    double Ith_Vbpbi;
    double Ith_Veie;
    double Ith_Vcic;
    double Ith_Vbbp;
    double dummy_1, dummy_2; //for debugging

    //declaration of lambda functions -----------------------------------

    //Hole charge at low bias
    std::function<duals::duald (duals::duald, duals::duald, duals::duald)> calc_Q_0 = [&](duals::duald Qjei, duals::duald Qjci, duals::duald hjei_vbe){
        duals::duald Q_0, b_q, Q_bpt ;
        a_bpt   = 0.05;
        Q_0     = here->HICUMqp0_t + hjei_vbe*Qjei + model->HICUMhjci*Qjci;
        Q_bpt   = a_bpt*here->HICUMqp0_t;
        b_q     = Q_0/Q_bpt-1;
        Q_0     = Q_bpt*(1+(b_q +sqrt(b_q*b_q+1.921812))/2);
        return Q_0;
    };

    std::function<duals::duald (duals::duald, duals::duald, duals::duald)> calc_T_f0 = [&](duals::duald T, duals::duald Vbici, duals::duald Qjci){
        //Transit time calculation at low current density
        duals::duald vt;
        duals::duald cV_f,cv_e,cs_q,cs_q2,cv_j,cdvj_dv,Cjcit,cc;

        vt = CONSTboltz * T / CHARGE;
        if(here->HICUMcjci0_t > 0.0){ // CJMODF
            cV_f    = here->HICUMvdci_t*(1.0-exp(-log(2.4)/model->HICUMzci));
            cv_e    = (cV_f-Vbici)/vt;
            cs_q    = sqrt(cv_e*cv_e+1.921812);
            cs_q2   = (cv_e+cs_q)*0.5;
            cv_j    = cV_f-vt*cs_q2;
            cdvj_dv = cs_q2/cs_q;
            Cjcit   = here->HICUMcjci0_t*exp(-model->HICUMzci*log(1.0-cv_j/here->HICUMvdci_t))*cdvj_dv+2.4*here->HICUMcjci0_t*(1.0-cdvj_dv);
        } else {
            Cjcit   = 0.0;
        }
        if(Cjcit > 0.0) {
            cc      = here->HICUMcjci0_t/Cjcit;
        } else {
            cc      = 1.0;
        }
        return here->HICUMt0_t+model->HICUMdt0h*(cc-1.0)+model->HICUMtbvl*(1/cc-1.0);
    };
    std::function<duals::duald (duals::duald, duals::duald)> calc_ick = [&](duals::duald T, duals::duald Vciei){
        duals::duald ick;
        duals::duald Ovpt,a,d1,vceff,a1,a11,Odelck,ick1,ick2,ICKa, vc, vt;
        //Effective collector voltage
        vc      = Vciei-here->HICUMvces_t;
        vt      = CONSTboltz * T / CHARGE;

        //Inverse of low-field internal collector resistance: needed in HICICK
        Orci0_t = 1.0/here->HICUMrci0_t;

        //Critical current for onset of high-current effects
        //begin : HICICK
            Ovpt    = 1.0/model->HICUMvpt;
            a       = vc/vt;
            d1      = a-1;
            vceff   = (1.0+((d1+sqrt(d1*d1+1.921812))/2))*vt;
            // a       = vceff/vlim_t;
            // ick     = vceff*Orci0_t/sqrt(1.0+a*a);
            // ICKa    = (vceff-vlim_t)*Ovpt;
            // ick     = ick*(1.0+0.5*(ICKa+sqrt(ICKa*ICKa+1.0e-3)));

            a1       = vceff/here->HICUMvlim_t;
            a11      = vceff*Orci0_t;
            Odelck   = 1/model->HICUMdelck;
            ick1     = exp(Odelck*log(1+exp(model->HICUMdelck*log(a1))));
            ick2     = a11/ick1;
            ICKa     = (vceff-here->HICUMvlim_t)*Ovpt;
            ick      = ick2*(1.0+0.5*(ICKa+sqrt(ICKa*ICKa+model->HICUMaick)));
            return ick;

        //end
    };

    std::function<duals::duald (duals::duald, duals::duald)> calc_ibet = [&](duals::duald Vbiei, duals::duald Vbpei){
        //Tunneling current
        duals::duald ibet;
        if (model->HICUMibets > 0 && (Vbpei <0.0 || Vbiei < 0.0)){ //begin : HICTUN
            duals::duald pocce,czz;
            if(model->HICUMtunode==1 && here->HICUMcjep0_t > 0.0 && here->HICUMvdep_t >0.0){
                pocce   = exp((1-1/model->HICUMzep)*log(Cjep/here->HICUMcjep0_t));
                czz     = -(Vbpei/here->HICUMvdep_t)*here->HICUMibets_t*pocce;
                ibet    = czz*exp(-here->HICUMabet_t/pocce);
            } else if (model->HICUMtunode==0 && here->HICUMcjei0_t > 0.0 && here->HICUMvdei_t >0.0){
                pocce   = exp((1-1/model->HICUMzei)*log(Cjei/here->HICUMcjei0_t));
                czz     = -(Vbiei/here->HICUMvdei_t)*here->HICUMibets_t*pocce;
                ibet    = czz*exp(-here->HICUMabet_t/pocce);
            } else {
                ibet    = 0.0;
            }
        } else {
            ibet    = 0.0;
        }
        return ibet;
    };

    std::function<duals::duald (duals::duald, duals::duald, duals::duald)> calc_iavl = [&](duals::duald Vbici, duals::duald Cjci, duals::duald itf){
        //Avalanche current
        iavl = 0;
        if (use_aval == 1) {//begin : HICAVL
            duals::duald v_bord,v_q,U0,av,avl,iavl;
            v_bord  = here->HICUMvdci_t-Vbici;
            if (v_bord > 0) {
                v_q     = here->HICUMqavl_t/Cjci;
                U0      = here->HICUMqavl_t/here->HICUMcjci0_t;
                if(v_bord > U0){
                    av      = here->HICUMfavl_t*exp(-v_q/U0);
                    avl     = av*(U0+(1.0+v_q/U0)*(v_bord-U0));
                } else {
                    avl     = here->HICUMfavl_t*v_bord*exp(-v_q/v_bord);
                }
                /* This model turns strong avalanche on. The parameter kavl can turn this
                * model extension off (kavl = 0). Although this is numerically stable, a
                * conditional statement is applied in order to reduce the numerical over-
                * head for simulations without the new model.
                */
                if (model->HICUMkavl > 0) { //: HICAVLHIGH
                    duals::duald denom,sq_smooth,hl;
                    denom = 1-here->HICUMkavl_t*avl;
                    // Avoid denom < 0 using a smoothing function
                    sq_smooth = sqrt(denom*denom+0.01);
                    hl        = 0.5*(denom+sq_smooth);
                    iavl      = itf*avl/hl;
                } else {
                    iavl    = itf*avl;
                }
            } else {
                iavl = 0.0;
            }
        }
        // Note that iavl = 0.0 is already set in the initialization block for use_aval == 0 (Markus: not for this lambda!)
        return iavl;
    };

    std::function<duals::duald (duals::duald, duals::duald, duals::duald, duals::duald)> calc_rbi = [&](duals::duald T, duals::duald Qjei, duals::duald Cjci, duals::duald Qf){
        //Internal base resistance
        duals::duald vt,rbi;
        vt      = CONSTboltz * T / CHARGE;
        if(here->HICUMrbi0_t > 0.0){ //: HICRBI
            duals::duald Qz_nom,f_QR,ETA,Qz0,fQz;
            // Consideration of conductivity modulation
            // To avoid convergence problem hyperbolic smoothing used
            f_QR    = (1+model->HICUMfdqr0)*here->HICUMqp0_t;
            Qz0     = Qjei+Qjci+Qf;
            Qz_nom  = 1+Qz0/f_QR;
            fQz     = 0.5*(Qz_nom+sqrt(Qz_nom*Qz_nom+0.01));
            rbi     = here->HICUMrbi0_t/fQz;
            // Consideration of emitter current crowding
            if( ibei > 0.0) {
                ETA     = rbi*ibei*model->HICUMfgeo/vt;
                if(ETA < 1.0e-6) {
                    rbi     = rbi*(1.0-0.5*ETA);
                } else {
                    rbi     = rbi*log(1.0+ETA)/ETA;
                }
            }
            // Consideration of peripheral charge
            if(Qf > 0.0) {
                rbi     = rbi*(Qjei+Qf*model->HICUMfqi)/(Qjei+Qf);
            }
         } else {
            rbi     = 0.0;
        }
        return rbi;
    };

    /*  loop through all the models */
    for (; model != NULL; model = HICUMnextModel(model)) {

        // Model_initialization

        // Depletion capacitance splitting at b-c junction
        // Capacitances at peripheral and external base node
        C_1 = (1.0 - model->HICUMfbcpar) *
                (model->HICUMcjcx0 + model->HICUMcbcpar);
        if (C_1 >= model->HICUMcbcpar) {
            cbcpar1 = model->HICUMcbcpar;
            cbcpar2 = 0.0;
            cjcx01 = C_1 - model->HICUMcbcpar;
            cjcx02 = model->HICUMcjcx0 - cjcx01;
        }
        else {
            cbcpar1 = C_1;
            cbcpar2 = model->HICUMcbcpar - cbcpar1;
            cjcx01 = 0.0;
            cjcx02 = model->HICUMcjcx0;
        }

        // Parasitic b-e capacitance partitioning: No temperature dependence
        cbepar2 = model->HICUMfbepar * model->HICUMcbepar;
        cbepar1 = model->HICUMcbepar - cbepar2;


        // Avoid divide-by-zero and define infinity other way
        // High current correction for 2D and 3D effects
        if (model->HICUMich != 0.0) {
            Oich = 1.0 / model->HICUMich;
        }
        else {
            Oich = 0.0;
        }

        // Base current recombination time constant at b-c barrier
        if (model->HICUMtbhrec != 0.0) {
            Otbhrec = 1.0 / model->HICUMtbhrec;
        }
        else {
            Otbhrec = 0.0;
        }

        // Turn avalanche calculation on depending of parameters
        if ((model->HICUMfavl > 0.0) && (model->HICUMcjci0 > 0.0)) {
            use_aval = 1;
        } else {
            use_aval = 0;
        }

// end of Model_initialization

        /* loop through all the instances of the model */
        for (here = HICUMinstances(model); here != NULL ;
                here=HICUMnextInstance(here)) {

            gqbepar1 = 0.0;
            gqbepar2 = 0.0;
            gqbcpar1 = 0.0;
            gqbcpar2 = 0.0;
            gqsu = 0.0;
            Icth = 0.0, Icth_Vrth = 0.0;

//            SCALE = here->HICUMarea * here->HICUMm;
            here->HICUMcbepar = model->HICUMcbepar;
            here->HICUMcbcpar = model->HICUMcbcpar;

            /*
             *   initialization
             */
            icheck=1;
            if(ckt->CKTmode & MODEINITSMSIG) {
                Vbiei = *(ckt->CKTstate0 + here->HICUMvbiei);
                Vbici = *(ckt->CKTstate0 + here->HICUMvbici);
                Vciei = Vbiei - Vbici;
                Vbpei = *(ckt->CKTstate0 + here->HICUMvbpei);
                Vbpci = *(ckt->CKTstate0 + here->HICUMvbpci);
                Vbci = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMbaseNode)-
                    *(ckt->CKTrhsOld+here->HICUMcollCINode));
                Vsici = *(ckt->CKTstate0 + here->HICUMvsici);
                Vsc = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMsubsNode)-
                    *(ckt->CKTrhsOld+here->HICUMcollNode));

                Vbpbi = *(ckt->CKTstate0 + here->HICUMvbpbi);
                Vbe = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMbaseNode)-
                    *(ckt->CKTrhsOld+here->HICUMemitNode));
                Vcic = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMcollCINode)-
                    *(ckt->CKTrhsOld+here->HICUMcollNode));
                Vbbp = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMbaseNode)-
                    *(ckt->CKTrhsOld+here->HICUMbaseBPNode));
                Vbpe = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMbaseBPNode)-
                    *(ckt->CKTrhsOld+here->HICUMemitNode));
                Veie = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMemitEINode)-
                    *(ckt->CKTrhsOld+here->HICUMemitNode));
                Vsis = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMsubsSINode)-
                    *(ckt->CKTrhsOld+here->HICUMsubsNode));
                Vbxf  = *(ckt->CKTrhsOld + here->HICUMxfNode);
                Vbxf1 = *(ckt->CKTrhsOld + here->HICUMxf1Node);
                Vbxf2 = *(ckt->CKTrhsOld + here->HICUMxf2Node);
                if (model->HICUMflsh)
                    Vrth = *(ckt->CKTstate0 + here->HICUMvrth);
            } else if(ckt->CKTmode & MODEINITTRAN) {
                Vbiei = *(ckt->CKTstate1 + here->HICUMvbiei);
                Vbici = *(ckt->CKTstate1 + here->HICUMvbici);
                Vciei = Vbiei - Vbici;
                Vbpei = *(ckt->CKTstate1 + here->HICUMvbpei);
                Vbpci = *(ckt->CKTstate1 + here->HICUMvbpci);
                Vbci = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMbaseNode)-
                    *(ckt->CKTrhsOld+here->HICUMcollCINode));
                Vsici = *(ckt->CKTstate1 + here->HICUMvsici);
                Vsc = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMsubsNode)-
                    *(ckt->CKTrhsOld+here->HICUMcollNode));

                Vbpbi = *(ckt->CKTstate1 + here->HICUMvbpbi);
                Vbe = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMbaseNode)-
                    *(ckt->CKTrhsOld+here->HICUMemitNode));
                Vcic = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMcollCINode)-
                    *(ckt->CKTrhsOld+here->HICUMcollNode));
                Vbbp = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMbaseNode)-
                    *(ckt->CKTrhsOld+here->HICUMbaseBPNode));
                Vbpe = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMbaseBPNode)-
                    *(ckt->CKTrhsOld+here->HICUMemitNode));
                Veie = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMemitEINode)-
                    *(ckt->CKTrhsOld+here->HICUMemitNode));
                Vsis = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMsubsSINode)-
                    *(ckt->CKTrhsOld+here->HICUMsubsNode));
                Vbxf  = *(ckt->CKTrhsOld + here->HICUMxfNode);
                Vbxf1 = *(ckt->CKTrhsOld + here->HICUMxf1Node);
                Vbxf2 = *(ckt->CKTrhsOld + here->HICUMxf2Node);
                if (model->HICUMflsh)
                    Vrth = *(ckt->CKTstate1 + here->HICUMvrth);
            } else if((ckt->CKTmode & MODEINITJCT) &&
                    (ckt->CKTmode & MODETRANOP) && (ckt->CKTmode & MODEUIC)){
                Vbe=Vbiei=model->HICUMtype*here->HICUMicVBE;
                Vciei=model->HICUMtype*here->HICUMicVCE;
                Vbci=Vbici=Vbpci=Vbiei-Vciei;
                Vbpei=0.0;
                Vsc=Vsici=0.0;
                Vbpbi=Vbbp=Vbpe=0.0;
                Vcic=Veie=Vsis=0.0;
                Vrth=0.0,Icth=0.0,Icth_Vrth=0.0;
                Vbxf=Vbxf1=Vbxf2=0.0;
            } else if((ckt->CKTmode & MODEINITJCT) && (here->HICUMoff==0)) {
                Vbe=Vbiei=model->HICUMtype*here->HICUMtVcrit;
                Vciei=0.0;
                Vbci=Vbici=Vbpci=0.0;
                Vbpei=0.0;
                Vsc=Vsici=0.0;
                Vbpbi=Vbbp=Vbpe=0.0;
                Vcic=Veie=Vsis=0.0;
                Vrth=0.0,Icth=0.0,Icth_Vrth=0.0;
                Vbxf=Vbxf1=Vbxf2=0.0;
            } else if((ckt->CKTmode & MODEINITJCT) ||
                    ( (ckt->CKTmode & MODEINITFIX) && (here->HICUMoff!=0))) {
                Vbe=0.0;
                Vbiei=Vbe;
                Vciei=0.0;
                Vbci=Vbici=Vbpci=0.0;
                Vbpei=0.0;
                Vsc=Vsici=0.0;
                Vbpbi=Vbbp=Vbpe=0.0;
                Vcic=Veie=Vsis=0.0;
                Vrth=0.0,Icth=0.0,Icth_Vrth=0.0;
                Vbxf=Vbxf1=Vbxf2=0.0;
            } else {
#ifndef PREDICTOR
                if(ckt->CKTmode & MODEINITPRED) {
                    xfact = ckt->CKTdelta/ckt->CKTdeltaOld[1];
                    Vbiei = (1+xfact) * *(ckt->CKTstate1 + here->HICUMvbiei)-
                            xfact * *(ckt->CKTstate2 + here->HICUMvbiei);
                    Vbici = (1+xfact) * *(ckt->CKTstate1 + here->HICUMvbici)-
                            xfact * *(ckt->CKTstate2 + here->HICUMvbici);
                    Vciei = Vbiei - Vbici;
                    Vbpei = (1+xfact) * *(ckt->CKTstate1 + here->HICUMvbpei)-
                            xfact * *(ckt->CKTstate2 + here->HICUMvbpei);
                    Vbpci = (1+xfact) * *(ckt->CKTstate1 + here->HICUMvbpci)-
                            xfact * *(ckt->CKTstate2 + here->HICUMvbpci);
                    Vsici = (1+xfact) * *(ckt->CKTstate1 + here->HICUMvsici)-
                            xfact * *(ckt->CKTstate2 + here->HICUMvsici);
                    Vbpbi = (1+xfact) * *(ckt->CKTstate1 + here->HICUMvbpbi)-
                            xfact * *(ckt->CKTstate2 + here->HICUMvbpbi);
                    Vbxf  = (1+xfact) * *(ckt->CKTstate1 + here->HICUMvxf)-
                            xfact * *(ckt->CKTstate2 + here->HICUMvxf);
                    Vbxf1 = (1+xfact) * *(ckt->CKTstate1 + here->HICUMvxf1)-
                            xfact * *(ckt->CKTstate2 + here->HICUMvxf1);
                    Vbxf2 = (1+xfact) * *(ckt->CKTstate1 + here->HICUMvxf2)-
                            xfact * *(ckt->CKTstate2 + here->HICUMvxf2);
                    *(ckt->CKTstate0 + here->HICUMvbiei) =
                            *(ckt->CKTstate1 + here->HICUMvbiei);
                    *(ckt->CKTstate0 + here->HICUMvbpei) =
                            *(ckt->CKTstate1 + here->HICUMvbpei);
                    *(ckt->CKTstate0 + here->HICUMvbici) =
                            *(ckt->CKTstate1 + here->HICUMvbici);
                    *(ckt->CKTstate0 + here->HICUMvbpei) =
                            *(ckt->CKTstate1 + here->HICUMvbpei);
                    *(ckt->CKTstate0 + here->HICUMvbpbi) =
                            *(ckt->CKTstate1 + here->HICUMvbpbi);
                    *(ckt->CKTstate0 + here->HICUMvsici) =
                            *(ckt->CKTstate1 + here->HICUMvsici);
                    *(ckt->CKTstate0 + here->HICUMvxf) =
                            *(ckt->CKTstate1 + here->HICUMvxf);
                    *(ckt->CKTstate0 + here->HICUMvxf1) =
                            *(ckt->CKTstate1 + here->HICUMvxf1);
                    *(ckt->CKTstate0 + here->HICUMvxf2) =
                            *(ckt->CKTstate1 + here->HICUMvxf2);
                    *(ckt->CKTstate0 + here->HICUMibiei) =
                            *(ckt->CKTstate1 + here->HICUMibiei);
                    *(ckt->CKTstate0 + here->HICUMibiei_Vbiei) =
                            *(ckt->CKTstate1 + here->HICUMibiei_Vbiei);
                    *(ckt->CKTstate0 + here->HICUMibpei) =
                            *(ckt->CKTstate1 + here->HICUMibpei);
                    *(ckt->CKTstate0 + here->HICUMibpei_Vbpei) =
                            *(ckt->CKTstate1 + here->HICUMibpei_Vbpei);
                    *(ckt->CKTstate0 + here->HICUMiciei) =
                            *(ckt->CKTstate1 + here->HICUMiciei);
                    *(ckt->CKTstate0 + here->HICUMiciei_Vbiei) =
                            *(ckt->CKTstate1 + here->HICUMiciei_Vbiei);
                    *(ckt->CKTstate0 + here->HICUMiciei_Vbici) =
                            *(ckt->CKTstate1 + here->HICUMiciei_Vbici);
                    *(ckt->CKTstate0 + here->HICUMibici) =
                            *(ckt->CKTstate1 + here->HICUMibici);
                    *(ckt->CKTstate0 + here->HICUMibici_Vbici) =
                            *(ckt->CKTstate1 + here->HICUMibici_Vbici);
                    *(ckt->CKTstate0 + here->HICUMibpei) =
                            *(ckt->CKTstate1 + here->HICUMibpei);
                    *(ckt->CKTstate0 + here->HICUMibpbi) =
                            *(ckt->CKTstate1 + here->HICUMibpbi);
                    *(ckt->CKTstate0 + here->HICUMibpbi_Vbpbi) =
                            *(ckt->CKTstate1 + here->HICUMibpbi_Vbpbi);
                    *(ckt->CKTstate0 + here->HICUMibpbi_Vbiei) =
                            *(ckt->CKTstate1 + here->HICUMibpbi_Vbiei);
                    *(ckt->CKTstate0 + here->HICUMibpbi_Vbici) =
                            *(ckt->CKTstate1 + here->HICUMibpbi_Vbici);
                    *(ckt->CKTstate0 + here->HICUMisici) =
                            *(ckt->CKTstate1 + here->HICUMisici);
                    *(ckt->CKTstate0 + here->HICUMisici_Vsici) =
                            *(ckt->CKTstate1 + here->HICUMisici_Vsici);
                    *(ckt->CKTstate0 + here->HICUMibpsi) =
                            *(ckt->CKTstate1 + here->HICUMibpsi);
                    *(ckt->CKTstate0 + here->HICUMibpsi_Vbpci) =
                            *(ckt->CKTstate1 + here->HICUMibpsi_Vbpci);
                    *(ckt->CKTstate0 + here->HICUMibpsi_Vsici) =
                            *(ckt->CKTstate1 + here->HICUMibpsi_Vsici);
                    *(ckt->CKTstate0 + here->HICUMgqbepar1) =
                            *(ckt->CKTstate1 + here->HICUMgqbepar1);
                    *(ckt->CKTstate0 + here->HICUMgqbepar2) =
                            *(ckt->CKTstate1 + here->HICUMgqbepar2);
                    *(ckt->CKTstate0 + here->HICUMieie) =
                            *(ckt->CKTstate1 + here->HICUMieie);
                    *(ckt->CKTstate0 + here->HICUMisis_Vsis) =
                            *(ckt->CKTstate1 + here->HICUMisis_Vsis);
//NQS
                    *(ckt->CKTstate0 + here->HICUMgqxf) =
                            *(ckt->CKTstate1 + here->HICUMgqxf);
                    *(ckt->CKTstate0 + here->HICUMixf_Vbiei) =
                            *(ckt->CKTstate1 + here->HICUMixf_Vbiei);
                    *(ckt->CKTstate0 + here->HICUMixf_Vbici) =
                            *(ckt->CKTstate1 + here->HICUMixf_Vbici);

                    if (model->HICUMflsh) {
                        Vrth = (1.0 + xfact)* (*(ckt->CKTstate1 + here->HICUMvrth))
                          - ( xfact * (*(ckt->CKTstate2 + here->HICUMvrth)));
                        *(ckt->CKTstate0 + here->HICUMvrth) =
                                *(ckt->CKTstate1 + here->HICUMvrth);
                        *(ckt->CKTstate0 + here->HICUMqcth) =
                                *(ckt->CKTstate1 + here->HICUMqcth);
                    }
                } else {
#endif /* PREDICTOR */
                    /*
                     *   compute new nonlinear branch voltages
                     */
                    Vbiei = model->HICUMtype*(
                        *(ckt->CKTrhsOld+here->HICUMbaseBINode)-
                        *(ckt->CKTrhsOld+here->HICUMemitEINode));
                    Vbici = model->HICUMtype*(
                        *(ckt->CKTrhsOld+here->HICUMbaseBINode)-
                        *(ckt->CKTrhsOld+here->HICUMcollCINode));
                    Vbpei = model->HICUMtype*(
                        *(ckt->CKTrhsOld+here->HICUMbaseBPNode)-
                        *(ckt->CKTrhsOld+here->HICUMemitEINode));
                    Vbpbi = model->HICUMtype*(
                        *(ckt->CKTrhsOld+here->HICUMbaseBPNode)-
                        *(ckt->CKTrhsOld+here->HICUMbaseBINode));
                    Vbpci = model->HICUMtype*(
                        *(ckt->CKTrhsOld+here->HICUMbaseBPNode)-
                        *(ckt->CKTrhsOld+here->HICUMcollCINode));
                    Vsici = model->HICUMtype*(
                        *(ckt->CKTrhsOld+here->HICUMsubsSINode)-
                        *(ckt->CKTrhsOld+here->HICUMcollCINode));
                    Vbxf  = *(ckt->CKTrhsOld + here->HICUMxfNode);
                    Vbxf1 = *(ckt->CKTrhsOld + here->HICUMxf1Node);
                    Vbxf2 = *(ckt->CKTrhsOld + here->HICUMxf2Node);
                    Vciei = Vbiei - Vbici;
                    if (model->HICUMflsh)
                        Vrth = *(ckt->CKTrhsOld + here->HICUMtempNode);
#ifndef PREDICTOR
                }
#endif /* PREDICTOR */
                delvbiei = Vbiei - *(ckt->CKTstate0 + here->HICUMvbiei);
                delvbici = Vbici - *(ckt->CKTstate0 + here->HICUMvbici);
                delvbpei = Vbpei - *(ckt->CKTstate0 + here->HICUMvbpei);
                delvbpbi = Vbpbi - *(ckt->CKTstate0 + here->HICUMvbpbi);
                delvbpci = Vbpci - *(ckt->CKTstate0 + here->HICUMvbpci);
                delvsici = Vsici - *(ckt->CKTstate0 + here->HICUMvsici);
                if (model->HICUMflsh)
                    delvrth = Vrth - *(ckt->CKTstate0 + here->HICUMvrth);
                Vbe = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMbaseNode)-
                    *(ckt->CKTrhsOld+here->HICUMemitNode));
                Vsc = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMsubsNode)-
                    *(ckt->CKTrhsOld+here->HICUMcollNode));
                Vcic = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMcollCINode)-
                    *(ckt->CKTrhsOld+here->HICUMcollNode));
                Vbci = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMbaseNode)-
                    *(ckt->CKTrhsOld+here->HICUMcollCINode));
                Vbbp = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMbaseNode)-
                    *(ckt->CKTrhsOld+here->HICUMbaseBPNode));
                Vbpe = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMbaseBPNode)-
                    *(ckt->CKTrhsOld+here->HICUMemitNode));
                Veie = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMemitEINode)-
                    *(ckt->CKTrhsOld+here->HICUMemitNode));
                Vsis = model->HICUMtype*(
                    *(ckt->CKTrhsOld+here->HICUMsubsSINode)-
                    *(ckt->CKTrhsOld+here->HICUMsubsNode));
                Vbxf = *(ckt->CKTrhsOld + here->HICUMxfNode);
                Vbxf1 = *(ckt->CKTrhsOld + here->HICUMxf1Node);
                Vbxf2 = *(ckt->CKTrhsOld + here->HICUMxf2Node);
                if (model->HICUMflsh)
                    Vrth = *(ckt->CKTrhsOld + here->HICUMtempNode);
                ibieihat = *(ckt->CKTstate0 + here->HICUMibiei) +
                         *(ckt->CKTstate0 + here->HICUMibiei_Vbiei)*delvbiei;
                ibicihat = *(ckt->CKTstate0 + here->HICUMibici) +
                         *(ckt->CKTstate0 + here->HICUMibici_Vbici)*delvbici;
                ibpeihat = *(ckt->CKTstate0 + here->HICUMibpei) +
                         *(ckt->CKTstate0 + here->HICUMibpei_Vbpei)*delvbpei;
                ibpcihat = *(ckt->CKTstate0 + here->HICUMibpci) +
                         *(ckt->CKTstate0 + here->HICUMibpci_Vbpci)*delvbpci;
                icieihat = *(ckt->CKTstate0 + here->HICUMiciei) +
                         *(ckt->CKTstate0 + here->HICUMiciei_Vbiei)*delvbiei +
                         *(ckt->CKTstate0 + here->HICUMiciei_Vbici)*delvbici;
                ibpbihat = *(ckt->CKTstate0 + here->HICUMibpbi) +
                         *(ckt->CKTstate0 + here->HICUMibpbi_Vbpbi)*delvbpbi +
                         *(ckt->CKTstate0 + here->HICUMibpbi_Vbiei)*delvbiei +
                         *(ckt->CKTstate0 + here->HICUMibpbi_Vbici)*delvbici;
                isicihat = *(ckt->CKTstate0 + here->HICUMisici) +
                         *(ckt->CKTstate0 + here->HICUMisici_Vsici)*delvsici;
                ibpsihat = *(ckt->CKTstate0 + here->HICUMibpsi) +
                         *(ckt->CKTstate0 + here->HICUMibpsi_Vbpci)*delvbpci +
                         *(ckt->CKTstate0 + here->HICUMibpsi_Vsici)*delvsici;
                /*
                 *    bypass if solution has not changed
                 */
                /* the following collections of if's would be just one
                 * if the average compiler could handle it, but many
                 * find the expression too complicated, thus the split.
                 * ... no bypass in case of selfheating
                 */
                if( (ckt->CKTbypass) && (!(ckt->CKTmode & MODEINITPRED)) && !model->HICUMflsh &&
                        (fabs(delvbiei) < (ckt->CKTreltol*MAX(fabs(Vbiei),
                            fabs(*(ckt->CKTstate0 + here->HICUMvbiei)))+
                            ckt->CKTvoltTol)) )
                    if( (fabs(delvbici) < ckt->CKTreltol*MAX(fabs(Vbici),
                            fabs(*(ckt->CKTstate0 + here->HICUMvbici)))+
                            ckt->CKTvoltTol) )
                    if( (fabs(delvbpei) < ckt->CKTreltol*MAX(fabs(Vbpei),
                            fabs(*(ckt->CKTstate0 + here->HICUMvbpei)))+
                            ckt->CKTvoltTol) )
                    if( (fabs(delvbpbi) < ckt->CKTreltol*MAX(fabs(Vbpbi),
                            fabs(*(ckt->CKTstate0 + here->HICUMvbpbi)))+
                            ckt->CKTvoltTol) )
                    if( (fabs(delvsici) < ckt->CKTreltol*MAX(fabs(Vsici),
                            fabs(*(ckt->CKTstate0 + here->HICUMvsici)))+
                            ckt->CKTvoltTol) )
                    if( (fabs(ibieihat-*(ckt->CKTstate0 + here->HICUMibiei)) <
                            ckt->CKTreltol* MAX(fabs(ibieihat),
                            fabs(*(ckt->CKTstate0 + here->HICUMibiei)))+
                            ckt->CKTabstol) )
                    if( (fabs(ibpeihat-*(ckt->CKTstate0 + here->HICUMibpei)) <
                            ckt->CKTreltol* MAX(fabs(ibpeihat),
                            fabs(*(ckt->CKTstate0 + here->HICUMibpei)))+
                            ckt->CKTabstol) )
                    if( (fabs(icieihat-*(ckt->CKTstate0 + here->HICUMiciei)) <
                            ckt->CKTreltol* MAX(fabs(icieihat),
                            fabs(*(ckt->CKTstate0 + here->HICUMiciei)))+
                            ckt->CKTabstol) )
                    if( (fabs(ibicihat-*(ckt->CKTstate0 + here->HICUMibici)) <
                            ckt->CKTreltol* MAX(fabs(ibicihat),
                            fabs(*(ckt->CKTstate0 + here->HICUMibici)))+
                            ckt->CKTabstol) )
                    if( (fabs(ibpcihat-*(ckt->CKTstate0 + here->HICUMibpei)) <
                            ckt->CKTreltol* MAX(fabs(ibpcihat),
                            fabs(*(ckt->CKTstate0 + here->HICUMibpei)))+
                            ckt->CKTabstol) )
                    if( (fabs(ibpbihat-*(ckt->CKTstate0 + here->HICUMibpbi)) <
                            ckt->CKTreltol* MAX(fabs(ibpbihat),
                            fabs(*(ckt->CKTstate0 + here->HICUMibpbi)))+
                            ckt->CKTabstol) )
                    if( (fabs(isicihat-*(ckt->CKTstate0 + here->HICUMisici)) <
                            ckt->CKTreltol* MAX(fabs(isicihat),
                            fabs(*(ckt->CKTstate0 + here->HICUMisici)))+
                            ckt->CKTabstol) )
                    if( (fabs(ibpsihat-*(ckt->CKTstate0 + here->HICUMibpsi)) <
                            ckt->CKTreltol* MAX(fabs(ibpsihat),
                            fabs(*(ckt->CKTstate0 + here->HICUMibpsi)))+
                            ckt->CKTabstol) ) {
                    /*
                     * bypassing....
                     */
                    Vbiei = *(ckt->CKTstate0 + here->HICUMvbiei);
                    Vbici = *(ckt->CKTstate0 + here->HICUMvbici);
                    Vbpei = *(ckt->CKTstate0 + here->HICUMvbpei);
                    Vbpbi = *(ckt->CKTstate0 + here->HICUMvbpbi);
                    Vbpci = *(ckt->CKTstate0 + here->HICUMvbpci);
                    Vsici = *(ckt->CKTstate0 + here->HICUMvsici);

                    Ibiei       = *(ckt->CKTstate0 + here->HICUMibiei);
                    Ibiei_Vbiei = *(ckt->CKTstate0 + here->HICUMibiei_Vbiei);

                    Ibpei       = *(ckt->CKTstate0 + here->HICUMibpei);
                    Ibpei_Vbpei = *(ckt->CKTstate0 + here->HICUMibpei_Vbpei);

                    Iciei       = *(ckt->CKTstate0 + here->HICUMiciei);
                    Iciei_Vbiei = *(ckt->CKTstate0 + here->HICUMiciei_Vbiei);
                    Iciei_Vbici = *(ckt->CKTstate0 + here->HICUMiciei_Vbici);

                    Ibici       = *(ckt->CKTstate0 + here->HICUMibici);
                    Ibici_Vbici = *(ckt->CKTstate0 + here->HICUMibici_Vbici);

                    Ibpbi       = *(ckt->CKTstate0 + here->HICUMibpbi);
                    Ibpbi_Vbpbi = *(ckt->CKTstate0 + here->HICUMibpbi_Vbpbi);
                    Ibpbi_Vbiei = *(ckt->CKTstate0 + here->HICUMibpbi_Vbiei);
                    Ibpbi_Vbici = *(ckt->CKTstate0 + here->HICUMibpbi_Vbici);

                    Isici       = *(ckt->CKTstate0 + here->HICUMisici);
                    Isici_Vsici = *(ckt->CKTstate0 + here->HICUMisici_Vsici);

                    Ibpsi       = *(ckt->CKTstate0 + here->HICUMibpsi);
                    Ibpsi_Vbpci = *(ckt->CKTstate0 + here->HICUMibpsi_Vbpci);
                    Ibpsi_Vsici = *(ckt->CKTstate0 + here->HICUMibpsi_Vsici);

                    Ibpci       = *(ckt->CKTstate0 + here->HICUMibpci);
                    Ibpci_Vbpci = *(ckt->CKTstate0 + here->HICUMibpci_Vbpci);

                    Ieie        = *(ckt->CKTstate0 + here->HICUMieie);

                    Isis_Vsis   = *(ckt->CKTstate0 + here->HICUMisis_Vsis);

                    gqbepar1    = *(ckt->CKTstate0 + here->HICUMgqbepar1);
                    gqbepar2    = *(ckt->CKTstate0 + here->HICUMgqbepar2);
                    gqbcpar1    = *(ckt->CKTstate0 + here->HICUMgqbcpar1);
                    gqbcpar2    = *(ckt->CKTstate0 + here->HICUMgqbcpar2);
                    goto load;
                }
                /*
                 *   limit nonlinear branch voltages
                 */
                ichk1 = 1, ichk2 = 1, ichk3 = 1, ichk4 = 1, ichk5 = 0;
                Vbiei = DEVpnjlim(Vbiei,*(ckt->CKTstate0 + here->HICUMvbiei),here->HICUMvt,
                        here->HICUMtVcrit,&icheck);
                Vbici = DEVpnjlim(Vbici,*(ckt->CKTstate0 + here->HICUMvbici),here->HICUMvt,
                        here->HICUMtVcrit,&ichk1);
                Vbpei = DEVpnjlim(Vbpei,*(ckt->CKTstate0 + here->HICUMvbpei),here->HICUMvt,
                        here->HICUMtVcrit,&ichk2);
                Vbpci = DEVpnjlim(Vbpci,*(ckt->CKTstate0 + here->HICUMvbpci),here->HICUMvt,
                        here->HICUMtVcrit,&ichk3);
                Vsici = DEVpnjlim(Vsici,*(ckt->CKTstate0 + here->HICUMvsici),here->HICUMvt,
                        here->HICUMtVcrit,&ichk4);
                if (model->HICUMflsh) {
                    ichk5 = 1;
                    Vrth = HICUMlimitlog(Vrth,
                        *(ckt->CKTstate0 + here->HICUMvrth),100,&ichk4);
                }
                if ((ichk1 == 1) || (ichk2 == 1) || (ichk3 == 1) || (ichk4 == 1) || (ichk5 == 1)) icheck=1;
            }
            /*
             *   determine dc current and derivatives
             */
//todo: check for double multiplication on pnp's
            Vbiei = model->HICUMtype*Vbiei;
            Vbici = model->HICUMtype*Vbici;
            Vciei = Vbiei-Vbici;
            Vbpei = model->HICUMtype*Vbpei;
            Vbpci = model->HICUMtype*Vbpci;
            Vbci  = model->HICUMtype*Vbci;
            Vsici = model->HICUMtype*Vsici;
            Vsc   = model->HICUMtype*Vsc;

            if (model->HICUMflsh!=0 && model->HICUMrth >= MIN_R) { // Thermal_update_with_self_heating
                here->HICUMtemp = here->HICUMtemp+Vrth;
                here->HICUMdtemp_sh = here->HICUMtemp - ckt->CKTtemp;
                iret = hicum_thermal_update(model, here);
            } else {
                here->HICUMdtemp_sh = 0;
            }

            // Model_evaluation

            //Intrinsic transistor
            //Internal base currents across b-e junction
            //TODO:derivative of ibeis_t and ireis_t missing here
            hicum_diode(here->HICUMtemp,here->HICUMibeis_t,model->HICUMmbei, Vbiei, &ibei, &ibei_Vbiei, &ibei_dT);
            hicum_diode(here->HICUMtemp,here->HICUMireis_t,model->HICUMmrei, Vbiei, &irei, &irei_Vbiei, &irei_dT);

            //Internal b-e and b-c junction capacitances and charges
            //QJMODF(here->HICUMvt,cjei0_t,vdei_t,model->HICUMzei,ajei_t,V(br_biei),Qjei)
            //Cjei    = ddx(Qjei,V(bi));
            //TODO: derivatives after cjei0_t, vdei_t ajei_t missing here
            hicum_qjmodf(here->HICUMtemp,here->HICUMcjei0_t,here->HICUMvdei_t,model->HICUMzei,here->HICUMajei_t,Vbiei,&Cjei,&Cjei_Vbiei, &Cjei_dT,&Qjei, &Qjei_Vbiei, &Qjei_dT);


            //TODO:missing temperature derivatives of vdei_t, hjei0_t vdei_t, ahjei_t
            result         = calc_hjei_vbe(Vbiei+1_e, here->HICUMtemp, here, model);
            hjei_vbe       = result.rpart();
            hjei_vbe_Vbiei = result.dpart();
            result         = calc_hjei_vbe(Vbiei, here->HICUMtemp+1_e, here, model);
            hjei_vbe_dT    = result.dpart();


            //HICJQ(here->HICUMvt,cjci0_t,vdci_t,model->HICUMzci,vptci_t,V(br_bici),Qjci);
            //Cjci    = ddx(Qjci,V(bi));
            //TODO: derivatives after cjci0_t, vdci_t, vptci_t
            hicum_HICJQ(here->HICUMtemp, here->HICUMcjci0_t,here->HICUMvdci_t,model->HICUMzci,here->HICUMvptci_t, Vbici, &Cjci, &Cjci_Vbici, &Cjci_dT, &Qjci, &Qjci_Vbici, &Qjci_dT);

            //Hole charge at low bias
            result    = calc_Q_0(Qjei+1_e, Qjci, hjei_vbe);
            Q_0       = result.rpart();
            Q_0_Qjei  = result.dpart();

            result    = calc_Q_0(Qjei, Qjci+1_e, hjei_vbe);
            Q_0_Qjci  = result.dpart();

            result       = calc_Q_0(Qjei, Qjci+1_e, hjei_vbe);
            Q_0_hjei_vbe = result.dpart();

            Q_0_Vbiei    = Q_0_Qjei*Qjei_Vbiei + Q_0_hjei_vbe*hjei_vbe_Vbiei;
            Q_0_Vbici    = Q_0_Qjci*Qjci_Vbici ;
            //TODO: derivative qp0_t
            Q_0_dT       = Q_0_Qjei*Qjei_dT + Q_0_Qjci*Qjci_dT * Q_0_hjei_vbe*hjei_vbe_dT;

            //Transit time calculation at low current density
            result      = calc_T_f0(here->HICUMtemp, Vbici+1_e, Qjci);
            T_f0        = result.rpart();
            T_f0_Vbici  = result.dpart();

            result      = calc_T_f0(here->HICUMtemp, Vbici, Qjci+1_e);
            T_f0_Qjci   = result.dpart();
            T_f0_Vbici += T_f0_Qjci*Qjci_Vbici;

            result      = calc_T_f0(here->HICUMtemp+1_e, Vbici, Qjci);
            T_f0_dT     = result.dpart() ;
            T_f0_dT    += T_f0_Qjci*Qjci_dT;


            //Critical current
            result      = calc_ick(here->HICUMtemp, Vciei+1_e);
            ick         = result.rpart();
            ick_Vciei   = result.dpart();

            //todo: derivatives rci0_t, vlim_t, vces_t missing
            result      = calc_ick(here->HICUMtemp+1_e, Vciei);
            ick_dT      = result.dpart();

            //Initialization
            //Transfer current, minority charges and transit times

            Tr      = model->HICUMtr;
            VT_f    = model->HICUMmcf*here->HICUMvt;
            i_0f    = here->HICUMc10_t * exp(Vbiei/VT_f);
            i_0f_Vbiei = i_0f/VT_f;
            i_0r    = here->HICUMc10_t * exp(Vbici/here->HICUMvt);
            i_0r_Vbici = i_0r/here->HICUMvt;

            //Initial formulation of forward and reverse component of transfer current
            Q_p     = Q_0;
            Q_p_Vbiei=Q_0_Vbiei;
            Q_p_Vbici=Q_0_Vbici;
            if (T_f0 > 0.0 || Tr > 0.0) {
                double A,A_Vbiei,A_Vbici,d1,d1_Vbiei,d1_Vbici;
                A       = 0.5*Q_0;
                A_Vbiei = 0.5*Q_0_Vbiei;
                A_Vbici = 0.5*Q_0_Vbici;
                d1      = sqrt(A*A+T_f0*i_0f+Tr*i_0r);
                d1_Vbiei= (2*A*A_Vbiei+T_f0*i_0f_Vbiei)/(2*d1);
                d1_Vbici= (2*A*A_Vbici+Tr*i_0r_Vbici)/(2*d1);
                Q_p     = A+d1;
                Q_p_Vbiei=A_Vbiei+d1_Vbiei;
                Q_p_Vbici=A_Vbici+d1_Vbici;
            }
            I_Tf1   = i_0f/Q_p;
            I_Tf1_Vbiei=(i_0f_Vbiei*Q_p-i_0f*Q_p_Vbiei)/(Q_p*Q_p);
            I_Tf1_Vbici=-i_0f*Q_p_Vbici/(Q_p*Q_p);
            a_h     = Oich*I_Tf1;
            itf     = I_Tf1*(1.0+a_h);
            itf_Vbiei=(Oich*I_Tf1+1.0)*I_Tf1_Vbiei+Oich*I_Tf1*I_Tf1_Vbiei;
            itf_Vbici=(Oich*I_Tf1+1.0)*I_Tf1_Vbici+Oich*I_Tf1*I_Tf1_Vbici;
            itr     = i_0r/Q_p;
            itr_Vbiei=-i_0r*Q_p_Vbiei/(Q_p*Q_p);
            itr_Vbici=(i_0r_Vbici*Q_p-i_0r*Q_p_Vbiei)/(Q_p*Q_p);

            //Initial formulation of forward transit time, diffusion, GICCR and excess b-c charge
            Q_bf    = 0.0;
            Tf      = T_f0;
            Qf      = T_f0*itf;
            //TODO
            //HICQFF(here, model, itf,ick,&Tf,&Qf,&T_fT,&Q_fT,&Q_bf);
//todo: itf=f(Vbiei,Vbici) -> Qf, Q_bf Ableitungen nach Vbiei, Vbici
            //Initial formulation of reverse diffusion charge
            Qr      = Tr*itr;

            //Preparation for iteration to get total hole charge and related variables
            l_it    = 0;
            if(Qf > RTOLC*Q_p || a_h > RTOLC) {
                //Iteration for Q_pT is required for improved initial solution
                Qf      = sqrt(T_f0*itf*Q_fT);
                Q_pT    = Q_0+Qf+Qr;
//todo: Q_pT_Vbiei, Vbici
                d_Q     = Q_pT;
                while (fabs(d_Q) >= RTOLC*fabs(Q_pT) && l_it <= l_itmax) {
                    double a;
                    I_Tf1   = i_0f/Q_pT;
                    a_h     = Oich*I_Tf1;
                    itf     = I_Tf1*(1.0+a_h);
                    itr     = i_0r/Q_pT;
                    Tf      = T_f0;
                    Qf      = T_f0*itf;
                    //TODO
                    //HICQFF(here, model, itf,ick,&Tf,&Qf,&T_fT,&Q_fT,&Q_bf);
                    Qr      = Tr*itr;
                    if(Oich == 0.0) {
                        a       = 1.0+(T_fT*itf+Qr)/Q_pT;
                    } else {
                        a       = 1.0+(T_fT*I_Tf1*(1.0+2.0*a_h)+Qr)/Q_pT;
                    }
                    d_Q     = -(Q_pT-(Q_0+Q_fT+Qr))/a;
                    //Limit maximum change of Q_pT
                    a       = fabs(0.3*Q_pT);
                    if(fabs(d_Q) > a) {
                        if (d_Q>=0) {
                            d_Q     = a;
                        } else {
                            d_Q     = -a;
                        }
                    }
                    Q_pT    = Q_pT+d_Q;
                    l_it    = l_it+1;
                } //while

                I_Tf1   = i_0f/Q_pT;
                a_h     = Oich*I_Tf1;
                itf     = I_Tf1*(1.0+a_h);
                itr     = i_0r/Q_pT;

                //Final transit times, charges and transport current components
                Tf      = T_f0;
                Qf      = T_f0*itf;
                //TODO
                //HICQFF(here, model, itf,ick,&Tf,&Qf,&T_fT,&Q_fT,&Q_bf);
                Qr      = Tr*itr;

            } //if
            itf_Vbiei = itf/VT_f;
            itr_Vbici = itr/here->HICUMvt;

            here->HICUMtf = Tf;

            //NQS effect implemented with LCR networks
            //Once the delay in ITF is considered, IT_NQS is calculated afterwards

            it      = itf-itr;

            //Diffusion charges for further use
            Qdei    = Qf;
            Qdci    = Qr;


            //High-frequency emitter current crowding (lateral NQS)
            Cdei    = T_f0*itf/here->HICUMvt;
            Cdci    = model->HICUMtr*itr/here->HICUMvt;
            Crbi    = model->HICUMfcrbi*(Cjei+Cjci+Cdei+Cdci);
            Qrbi    = Crbi*Vbpbi;
            Qrbi_Vbpbi = Crbi;
            Qrbi_Vbiei = Vbpbi*model->HICUMfcrbi*(T_f0*itf_Vbiei+Cjei_Vbiei);
            Qrbi_Vbici = Vbpbi*model->HICUMfcrbi*(model->HICUMtr*itr_Vbici+Cjci_Vbici);

            // Qrbi = model->HICUMfcrbi*(Qjei+Qjci+Qdei+Qdci);

            //HICCR: }

            //Internal base current across b-c junction
            //TODO
            hicum_diode(here->HICUMtemp,here->HICUMibcis_t,model->HICUMmbci, Vbici, &ibci, &ibci_Vbci, &ibci_dT);

            //Avalanche current
            result      = calc_iavl(Vbici+1_e, Cjci    , itf);
            iavl        = result.rpart();
            iavl_Vbici  = result.dpart();
            result      = calc_iavl(Vbici    , Cjci+1_e, itf);
            iavl_dCjci  = result.dpart();
            result      = calc_iavl(Vbici    , Cjci    , itf+1_e);
            iavl_ditf   = result.dpart();
            iavl_Vbici += iavl_ditf*itf_Vbici;
            iavl_Vbiei += iavl_ditf*itf_Vbiei;
            iavl_dT     = iavl_ditf*itf_dT    + iavl_dCjci*Cjci_dT; //TODO: derivatives kavl_t favl_t qavl_t cjci0_t vdci_t

            here->HICUMiavl = iavl;

            //Excess base current from recombination at the b-c barrier
            ibh_rec = Q_bf*Otbhrec;
//todo: Q_bf derivatives to Vbiei
            ibh_rec_Vbiei = 0.0;

            //internal base resistance
            result    = calc_rbi(here->HICUMtemp+1_e, Qjei    , Cjci    , Qf    );
            rbi       = result.rpart();
            rbi_dT    = result.dpart();
            result    = calc_rbi(here->HICUMtemp    , Qjei+1_e, Cjci    , Qf    );
            rbi_dQjei = result.dpart();
            result    = calc_rbi(here->HICUMtemp    , Qjei    , Cjci+1_e, Qf    );
            rbi_dCjci = result.dpart();
            result    = calc_rbi(here->HICUMtemp    , Qjei    , Cjci    , Qf+1_e);
            rbi_dQf   = result.dpart();
            here->HICUMrbi = rbi;

            rbi_Vbiei = rbi_dQjei* Qjei_Vbiei  + rbi_dQf  *Qf_Vbiei                  ;
            rbi_Vbici = rbi_dQf  * Qf_Vbici    + rbi_dCjci*Cjci_Vbici                ;
            rbi_dT   += rbi_dQjei*Qjei_dT      + rbi_dCjci*Cjci_dT    + rbi_dQf*Qf_dT;

            //Base currents across peripheral b-e junction
            //TODO: temperature derivative with ibeps_t ireps_t
            hicum_diode(here->HICUMtemp,here->HICUMibeps_t,model->HICUMmbep, Vbpei, &ibep, &ibep_Vbpei, &ibep_dT);
            hicum_diode(here->HICUMtemp,here->HICUMireps_t,model->HICUMmrep, Vbpei, &irep, &irep_Vbpei, &irep_dT);

            //Peripheral b-e junction capacitance and charge
            //TODO: derivatives with cjep0_t vdep_t
            hicum_qjmodf(here->HICUMtemp,here->HICUMcjep0_t,here->HICUMvdep_t,model->HICUMzep,here->HICUMajep_t,Vbpei,&Cjep,&Cjep_Vbpei, &Cjep_dT,&Qjep, &Qjep_Vbpei, &Qjep_dT);

            //Tunneling current
            //TODO: missing temperature derivatives abet_t vdei_t ibets_t cjei0_t vdep_t ibets_t cjep0_t
            result      = calc_ibet(Vbiei, Vbpei+1_e);
            ibet        = result.rpart();
            ibet_Vbpei  = result.dpart();

            result      = calc_ibet(Vbiei+1_e, Vbpei);
            ibet_Vbiei  = result.dpart();
            ibet_dT     = 0;


            //Base currents across peripheral b-c junction (bp,ci)
            hicum_diode(here->HICUMtemp,here->HICUMibcxs_t,model->HICUMmbcx, Vbpci, &ijbcx, &ijbcx_Vbpci, &ijbcx_dT);

            //Depletion capacitance and charge at external b-c junction (b,ci)
            //TODO: derivatives after cjcx01_t, vdcx_t, vptcx_t
            hicum_HICJQ(here->HICUMtemp, here->HICUMcjcx01_t,here->HICUMvdcx_t,model->HICUMzcx,here->HICUMvptcx_t, Vbci, &Cjcx_i, &Cjcx_i_Vbci, &Cjcx_i_dT, &Qjcx_i, &Qjcx_i_Vbci, &Qjcx_i_dT);

            //Depletion capacitance and charge at peripheral b-c junction (bp,ci)
            //TODO: derivatives after cjcx02_t, vdcx_t, vptcx_t
            hicum_HICJQ(here->HICUMtemp, here->HICUMcjcx02_t,here->HICUMvdcx_t,model->HICUMzcx,here->HICUMvptcx_t, Vbpci, &Cjcx_ii, &Cjcx_ii_Vbpci, &Cjcx_ii_dT, &Qjcx_ii, &Qjcx_ii_Vbpci, &Qjcx_ii_dT);

            //Depletion substrate capacitance and charge at inner s-c junction (si,ci)
            //TODO
            //HICJQ(here->HICUMvt,here->HICUMcjs0_t,here->HICUMvds_t,model->HICUMzs,here->HICUMvpts_t,Vsici,&Cjs,&Cjs_Vsici,&Qjs);
            /* Peripheral substrate capacitance and charge at s-c junction (s,c)
             * Bias dependent only if model->HICUMvdsp > 0
             */
            if (model->HICUMvdsp > 0) {
                //TODO
                //HICJQ(here->HICUMvt,here->HICUMcscp0_t,here->HICUMvdsp_t,model->HICUMzsp,here->HICUMvptsp_t,Vsc,&Cscp,&Cscp_Vsc,&Qscp);
            } else {
                // Constant, temperature independent capacitance
                Cscp = model->HICUMcscp0;
                Qscp = model->HICUMcscp0*Vsc;
            }

            //Parasitic substrate transistor transfer current and diffusion charge
            if(model->HICUMitss > 0.0) { // Sub_Transfer
                double HSa,HSb;
                HSUM    = model->HICUMmsf*here->HICUMvt;
                HSa     = exp(Vbpci/HSUM);
                HSb     = exp(Vsici/HSUM);
                HSI_Tsu = here->HICUMitss_t*(HSa-HSb);
                Ibpsi_Vbpci =  here->HICUMitss_t*HSa/HSUM;
                Ibpsi_Vsici = -here->HICUMitss_t*HSb/HSUM;
                if(model->HICUMtsf > 0.0) {
                    Qdsu = here->HICUMtsf_t*here->HICUMitss_t*HSa;
                    Qdsu_Vbpci = here->HICUMtsf_t*here->HICUMitss_t*HSa/HSUM;
                } else {
                    Qdsu = 0.0;
                    Qdsu_Vbpci = 0.0;
                }
            } else {
                HSI_Tsu = 0.0;
                Ibpsi_Vbpci = 0.0;
                Ibpsi_Vsici = 0.0;
                Qdsu = 0.0;
                Qdsu_Vbpci = 0.0;
            }

            // Current gain computation for correlated noise implementation
            if (ibei > 0.0) {
                here->HICUMbetadc=it/ibei;
            } else {
                here->HICUMbetadc=0.0;
            }
            Ieie = Veie/here->HICUMre_t; // only needed for re flicker noise

            //Diode current for s-c junction (si,ci)
            //TODO
            //HICDIO(here->HICUMvt,model->HICUMiscs,here->HICUMiscs_t,model->HICUMmsc,Vsici,&ijsc,&Isici_Vsici);

            //Self-heating calculation
            if (model->HICUMflsh == 1 && model->HICUMrth >= MIN_R) {
                pterm   =  Vciei*it + (here->HICUMvdci_t-Vbici)*iavl;
            } else if (model->HICUMflsh == 2 && model->HICUMrth >= MIN_R) {
                pterm   =  Vciei*it + (here->HICUMvdci_t-Vbici)*iavl + ibei*Vbiei + ibci*Vbici + ibep*Vbpei + ijbcx*Vbpci + ijsc*Vsici;
                if (rbi >= MIN_R) {
                    pterm   = pterm + Vbpbi*Vbpbi/rbi;
                }
                if (here->HICUMre_t >= MIN_R) {
                    pterm   = pterm + Veie*Veie/here->HICUMre_t;
                }
                if (here->HICUMrcx_t >= MIN_R) {
                    pterm   = pterm + Vcic*Vcic/here->HICUMrcx_t;
                }
                if (here->HICUMrbx_t >= MIN_R) {
                    pterm   = pterm + Vbbp*Vbbp/here->HICUMrbx_t;
                }
            } else {
                pterm = 0; // default value...
            }
            here->HICUMpterm = pterm;

            Itxf    = itf;
            Qdeix   = Qdei;

            // Excess Phase calculation

            if ((model->HICUMflnqs != 0 || model->HICUMflcomp == 0.0 || model->HICUMflcomp == 2.1) && Tf != 0 && (model->HICUMalit > 0 || model->HICUMalqf > 0)) {
                Vxf1  = Vbxf1;
                Vxf2  = Vbxf2;

                Ixf1  = (Vxf2-itf)/Tf*model->HICUMt0;
                Ixf2  = (Vxf2-Vxf1)/Tf*model->HICUMt0;
                Qxf1      = model->HICUMalit*model->HICUMt0*Vxf1;
                Qxf1_Vxf1 = model->HICUMalit*model->HICUMt0;
                Qxf2      = model->HICUMalit*model->HICUMt0*Vxf2/3;
                Qxf2_Vxf2 = model->HICUMalit*model->HICUMt0/3;
                Itxf  = Vxf2;

                // TODO derivatives of Ixf1 and Ixf2

                Vxf   = Vbxf;                                //for RC nw
                Ixf   = (Vxf - Qdei)*model->HICUMt0/Tf;      //for RC nw
                Qxf     = model->HICUMalqf*model->HICUMt0*Vxf; //for RC nw
                Qxf_Vxf = model->HICUMalqf*model->HICUMt0;   //for RC nw
                Qdeix = Vxf;                                 //for RC nw
            } else {
                Ixf1  =  Vbxf1;
                Ixf2  =  Vbxf2;
                Qxf1  =  0;
                Qxf2  =  0;
                Qxf1_Vxf1 = 0;
                Qxf2_Vxf2 = 0;

                Ixf   = Vbxf;
                Qxf   = 0;
                Qxf_Vxf = 0;
            }

            // end of Model_evaluation

            // Load_sources

            Ibpei       = model->HICUMtype*ibep;
            Ibpei      += model->HICUMtype*irep;
            Ibpei_Vbpei += model->HICUMtype*irep_Vbpei;

            Ibiei        = model->HICUMtype*ibei;
            Ibiei_Vbiei  = model->HICUMtype*ibei_Vbiei;
            Ibiei       += model->HICUMtype*irei;
            Ibiei_Vbiei += model->HICUMtype*irei_Vbiei;
            Ibiei       += model->HICUMtype*ibh_rec;
            Ibiei_Vbiei += model->HICUMtype*ibh_rec_Vbiei;

            if (model->HICUMtunode==1.0) {
                Ibpei  += -model->HICUMtype*ibet;
                Ibpei_Vbpei += -model->HICUMtype*ibet_Vbpei;
            } else {
                Ibiei  += -model->HICUMtype*ibet;
                Ibiei_Vbiei += -model->HICUMtype*ibet_Vbiei;
            }
//printf("Vbiei: %f ibei: %g irei: %g ibh_rec: %g ibet: %g\n",Vbiei,ibei,irei,ibh_rec,ibet);
            Ibpsi       = model->HICUMtype*HSI_Tsu;

            Ibpci       = model->HICUMtype*ijbcx;

            Ibici = model->HICUMtype*(ibci - iavl);
            Ibici_Vbici = model->HICUMtype*(Ibici_Vbici - iavl_Vbici);

            Isici       = model->HICUMtype*ijsc;

            Iciei       =  model->HICUMtype*(Itxf - itr);
            Iciei_Vbiei =  model->HICUMtype*itf_Vbiei;
            Iciei_Vbici = -model->HICUMtype*itr_Vbici;

//printf("Vbiei: %f Vbici: %f Vciei: %f Vbpei: %f Vbpci: %f Vbci: %f Vsici: %f\n", Vbiei, Vbici, Vciei, Vbpei, Vbpci, Vbci, Vsici);
//printf("Ibiei: %g Ibici: %g Ibpei: %g Iciei: %g\n",Ibiei,Ibici,Ibpei,Iciei);

            // Following code is an intermediate solution (if branch contribution is not supported):
            // ******************************************
            //if(model->HICUMflsh == 0 || model->HICUMrth < MIN_R) {
            //      I[br_sht]       <+ Vrth/MIN_R;
            //} else {
            //      I[br_sht]       <+ Vrth/rth_t-pterm;
            //      I[br_sht]       <+ ddt(model->HICUMcth*Vrth]);
            //}

            // ******************************************

            // For simulators having no problem with Vrth) <+ 0.0
            // with external thermal node, following code may be used.
            // Note that external thermal node should remain accessible
            // even without self-heating.
            // ********************************************
            Ith_Vbiei  = 0.0;
            Ith_Vbici  = 0.0;
            Ith_Vbpbi  = 0.0;
            Ith_Vbpci  = 0.0;
            Ith_Vbpei  = 0.0;
            Ith_Vciei  = 0.0;
            Ith_Vsici  = 0.0;
            Ith_Vcic   = 0.0;
            Ith_Vbbp   = 0.0;
            Ith_Veie   = 0.0;
            Ith_Vrth   = 0.0;
            if(model->HICUMflsh == 0 || model->HICUMrth < MIN_R) {
                Ith       = 0.0;
            } else {
                Ith       = Vrth/here->HICUMrth_t-pterm;
                if (model->HICUMflsh == 1 && model->HICUMrth >= MIN_R) {
                    Ith_Vciei  = -it;
                    Ith_Vbici  =  iavl;
                } else if (model->HICUMflsh == 2 && model->HICUMrth >= MIN_R) {
                    Ith_Vciei  = -it;
                    Ith_Vbiei  = -ibei;
                    Ith_Vbici  = -ibci+iavl;
                    Ith_Vbpei  = -ibep;
                    Ith_Vbpci  = -ijbcx;
                    Ith_Vsici  = -ijsc;
                    if (rbi >= MIN_R) {
                        Ith_Vbpbi  = -Vbpbi*Vbpbi/rbi;
                    }
                    if (here->HICUMre_t >= MIN_R) {
                        Ith_Veie   = -Veie*Veie/here->HICUMre_t;
                    }
                    if (here->HICUMrcx_t >= MIN_R) {
                        Ith_Vcic   = -Vcic*Vcic/here->HICUMrcx_t;
                    }
                    if (here->HICUMrbx_t >= MIN_R) {
                        Ith_Vbbp   = -Vbbp*Vbbp/here->HICUMrbx_t;
                    }
                }
            }
            // ********************************************

            // NQS effect
        //    Ibxf1 = Ixf1;
//            Icxf1 += ddt(Qxf1);
        //    Ibxf2 = Ixf2;
//            Icxf2 += ddt(Qxf2);

            // end of Load_sources

            if (rbi >= MIN_R) {
                Ibpbi_Vbpbi = 1 / rbi;
                Ibpbi = Vbpbi / rbi;
            } else {
                // Makrus: why this???
                Ibpbi_Vbpbi = 1 / MIN_R;
                Ibpbi = Vbpbi / MIN_R;

                // Ibpbi = 0;
                // Ibpbi_Vbpbi = 0;
            }
            Ibpbi_Vbiei = -Vbpbi * rbi_Vbiei / (rbi*rbi);
            Ibpbi_Vbici = -Vbpbi * rbi_Vbici / (rbi*rbi);

            Ibbp_Vbbp    = 1/here->HICUMrbx_t;
            Icic_Vcic    = 1/here->HICUMrcx_t;
            Ieie_Veie    = 1/here->HICUMre_t;
            Isis_Vsis    = 1/model->HICUMrsu;

            qjcx0_t_i_Vbci   = Cjcx_i;
            qjcx0_t_ii_Vbpci = Cjcx_ii;
            Qjep_Vbpei       = Cjep;
            Qdeix_Vbiei      = Cdei;
            Qdci_Vbici       = Cdci;
            Qbepar1_Vbe      = cbepar1;
            Qbepar2_Vbpe     = cbepar2;
            Qbcpar1_Vbci     = cbcpar1;
            Qbcpar2_Vbpci    = cbcpar2;
            Qsu_Vsis         = model->HICUMcsu;
            Qjs_Vsici        = Cjs;

//todo: all the derivatives have to be known dI/dT
            Ibbp_Vrth = 0.0;
            Ieie_Vrth = 0.0;
            Icic_Vrth = 0.0;
            Irth_Vrth = 0.0;
            Ibici_Vrth = 0.0;
            Ibpei_Vrth = 0.0;
            Ibiei_dT   = 0.0;
            Ibpci_Vrth = 0.0;
            Ibpbi_Vrth = 0.0;
            Iciei_Vrth = 0.0;
            Isici_Vrth = 0.0;

//todo: what about dQ/dT ?

            Ibiei += ckt->CKTgmin*Vbiei;
            Ibiei_Vbiei += ckt->CKTgmin;
            Ibici += ckt->CKTgmin*Vbici;
            Ibici_Vbici += ckt->CKTgmin;
            Iciei += ckt->CKTgmin*Vciei;
            Iciei_Vbiei += ckt->CKTgmin;
            Iciei_Vbici += ckt->CKTgmin;
            Ibpei += ckt->CKTgmin*Vbpei;
            Ibpei_Vbpei += ckt->CKTgmin;
            Ibpbi += ckt->CKTgmin*Vbpbi;
            Ibpbi_Vbiei += ckt->CKTgmin;
            Ibpbi_Vbici += ckt->CKTgmin;
            Ibpci += ckt->CKTgmin*Vbpci;
            Ibpci_Vbpci += ckt->CKTgmin;
            Isici += ckt->CKTgmin*Vsici;
            Isici_Vsici += ckt->CKTgmin;

            if( (ckt->CKTmode & (MODEDCTRANCURVE | MODETRAN | MODEAC)) ||
                    ((ckt->CKTmode & MODETRANOP) && (ckt->CKTmode & MODEUIC)) ||
                    (ckt->CKTmode & MODEINITSMSIG)) {
                /*
                 *   charge storage elements
                 */
                Qbepar1=cbepar1*Vbe;
                Qbepar2=cbepar2*Vbpe;
                Qbcpar1=cbcpar1*Vbci;
                Qbcpar2=cbcpar2*Vbpci;
                Qsu=model->HICUMcsu*Vsis;
                Qcth=model->HICUMcth*Vrth;

                *(ckt->CKTstate0 + here->HICUMqrbi)     = Qrbi;
                *(ckt->CKTstate0 + here->HICUMqdeix)    = Qdeix;
                *(ckt->CKTstate0 + here->HICUMqjei)     = Qjei;
                *(ckt->CKTstate0 + here->HICUMqdci)     = Qdci;
                *(ckt->CKTstate0 + here->HICUMqjci)     = Qjci;
                *(ckt->CKTstate0 + here->HICUMqjep)     = Qjep;
                *(ckt->CKTstate0 + here->HICUMqjcx0_i)  = qjcx0_t_i;
                *(ckt->CKTstate0 + here->HICUMqjcx0_ii) = qjcx0_t_ii;
                *(ckt->CKTstate0 + here->HICUMqdsu)     = Qdsu;
                *(ckt->CKTstate0 + here->HICUMqjs)      = Qjs;
                *(ckt->CKTstate0 + here->HICUMqscp)     = Qscp;
                *(ckt->CKTstate0 + here->HICUMqbepar1)  = Qbepar1;
                *(ckt->CKTstate0 + here->HICUMqbepar2)  = Qbepar2;
                *(ckt->CKTstate0 + here->HICUMqbcpar1)  = Qbcpar1;
                *(ckt->CKTstate0 + here->HICUMqbcpar2)  = Qbcpar2;
                *(ckt->CKTstate0 + here->HICUMqsu)      = Qsu;
//NQS
                *(ckt->CKTstate0 + here->HICUMqxf1)     = Qxf1;
                *(ckt->CKTstate0 + here->HICUMqxf2)     = Qxf2;
                *(ckt->CKTstate0 + here->HICUMqxf)      = Qxf;
                if (model->HICUMflsh)
                    *(ckt->CKTstate0 + here->HICUMqcth) = Qcth;

                here->HICUMcaprbi      = Qrbi_Vbpbi;
                here->HICUMcapdeix     = Cdei;
                here->HICUMcapjei      = Cjei;
                here->HICUMcapdci      = Cdci;
                here->HICUMcapjci      = Cjci;
                here->HICUMcapjep      = Cjep;
                here->HICUMcapjcx_t_i  = Cjcx_i;
                here->HICUMcapjcx_t_ii = Cjcx_ii;
                here->HICUMcapdsu      = Qdsu_Vbpci;
                here->HICUMcapjs       = Cjs;
                here->HICUMcapscp      = Cscp;
                here->HICUMcapsu       = model->HICUMcsu;
                here->HICUMcapcth      = model->HICUMcth;
                here->HICUMcapscp      = Cscp;

                /*
                 *   store small-signal parameters
                 */
                if ( (!(ckt->CKTmode & MODETRANOP))||
                        (!(ckt->CKTmode & MODEUIC)) ) {
                    if(ckt->CKTmode & MODEINITSMSIG) {
                        *(ckt->CKTstate0 + here->HICUMcqrbi)      = Qrbi_Vbpbi;
                        *(ckt->CKTstate0 + here->HICUMcqdeix)     = Qdeix_Vbiei;
                        *(ckt->CKTstate0 + here->HICUMcqjei)      = Cjei;
                        *(ckt->CKTstate0 + here->HICUMcqdci)      = Qdci_Vbici;
                        *(ckt->CKTstate0 + here->HICUMcqjci)      = Cjci;
                        *(ckt->CKTstate0 + here->HICUMcqjep)      = Qjep_Vbpei;
                        *(ckt->CKTstate0 + here->HICUMcqcx0_t_i)  = qjcx0_t_i_Vbci;
                        *(ckt->CKTstate0 + here->HICUMcqcx0_t_ii) = qjcx0_t_ii_Vbpci;
                        *(ckt->CKTstate0 + here->HICUMcqdsu)      = Qdsu_Vbpci;
                        *(ckt->CKTstate0 + here->HICUMcqjs)       = Qjs_Vsici;
                        *(ckt->CKTstate0 + here->HICUMcqscp)      = Cscp;
                        *(ckt->CKTstate0 + here->HICUMcqbepar1)   = Qbepar1_Vbe;
                        *(ckt->CKTstate0 + here->HICUMcqbepar2)   = Qbepar2_Vbpe;
                        *(ckt->CKTstate0 + here->HICUMcqbcpar1)   = Qbcpar1_Vbci;
                        *(ckt->CKTstate0 + here->HICUMcqbcpar2)   = Qbcpar2_Vbpci;
                        *(ckt->CKTstate0 + here->HICUMcqsu)       = Qsu_Vsis;
//NQS
                        *(ckt->CKTstate0 + here->HICUMcqxf1)      = Qxf1_Vxf1;
                        *(ckt->CKTstate0 + here->HICUMcqxf2)      = Qxf2_Vxf2;
                        *(ckt->CKTstate0 + here->HICUMcqxf)       = Qxf_Vxf;
                        if (model->HICUMflsh)
                            *(ckt->CKTstate0 + here->HICUMcqcth)  = model->HICUMcth;
                        continue; /* go to 1000 */
                    }
                    /*
                     *   transient analysis
                     */
                    if(ckt->CKTmode & MODEINITTRAN) {
                        *(ckt->CKTstate1 + here->HICUMqrbi) =
                                *(ckt->CKTstate0 + here->HICUMqrbi) ;
                        *(ckt->CKTstate1 + here->HICUMqjei) =
                                *(ckt->CKTstate0 + here->HICUMqjei) ;
                        *(ckt->CKTstate1 + here->HICUMqdeix) =
                                *(ckt->CKTstate0 + here->HICUMqdeix) ;
                        *(ckt->CKTstate1 + here->HICUMqjci) =
                                *(ckt->CKTstate0 + here->HICUMqjci) ;
                        *(ckt->CKTstate1 + here->HICUMqdci) =
                                *(ckt->CKTstate0 + here->HICUMqdci) ;
                        *(ckt->CKTstate1 + here->HICUMqjep) =
                                *(ckt->CKTstate0 + here->HICUMqjep) ;
                        *(ckt->CKTstate1 + here->HICUMqjcx0_i) =
                                *(ckt->CKTstate0 + here->HICUMqjcx0_i) ;
                        *(ckt->CKTstate1 + here->HICUMqjcx0_ii) =
                                *(ckt->CKTstate0 + here->HICUMqjcx0_ii) ;
                        *(ckt->CKTstate1 + here->HICUMqdsu) =
                                *(ckt->CKTstate0 + here->HICUMqdsu) ;
                        *(ckt->CKTstate1 + here->HICUMqjs) =
                                *(ckt->CKTstate0 + here->HICUMqjs) ;
                        *(ckt->CKTstate1 + here->HICUMqscp) =
                                *(ckt->CKTstate0 + here->HICUMqscp) ;
                        *(ckt->CKTstate1 + here->HICUMqbepar1) =
                                *(ckt->CKTstate0 + here->HICUMqbepar1) ;
                        *(ckt->CKTstate1 + here->HICUMqbepar2) =
                                *(ckt->CKTstate0 + here->HICUMqbepar2) ;
                        *(ckt->CKTstate1 + here->HICUMqbcpar1) =
                                *(ckt->CKTstate0 + here->HICUMqbcpar1) ;
                        *(ckt->CKTstate1 + here->HICUMqbcpar2) =
                                *(ckt->CKTstate0 + here->HICUMqbcpar2) ;
                        *(ckt->CKTstate1 + here->HICUMqsu) =
                                *(ckt->CKTstate0 + here->HICUMqsu) ;
//NQS
                        *(ckt->CKTstate1 + here->HICUMqxf) =
                                *(ckt->CKTstate0 + here->HICUMqxf) ;
                        if (model->HICUMflsh)
                            *(ckt->CKTstate1 + here->HICUMqcth) =
                                    *(ckt->CKTstate0 + here->HICUMqcth) ;
                    }

//            Ibpbi      += ddt(Qrbi);
                    error = NIintegrate(ckt,&geq,&ceq,Qrbi_Vbpbi,here->HICUMqrbi);
                    if(error) return(error);
                    Ibpbi_Vbpbi += geq;
                    Ibpbi += *(ckt->CKTstate0 + here->HICUMcqrbi);

//            Ibiei      += ddt(model->HICUMtype*(Qdeix+Qjei));
                    error = NIintegrate(ckt,&geq,&ceq,Cdei,here->HICUMqdeix);
                    if(error) return(error);
                    Ibiei_Vbiei += geq;
                    Ibiei += *(ckt->CKTstate0 + here->HICUMcqdeix);

                    error = NIintegrate(ckt,&geq,&ceq,Cjei,here->HICUMqjei);
                    if(error) return(error);
                    Ibiei_Vbiei += geq;
                    Ibiei += *(ckt->CKTstate0 + here->HICUMcqjep);

//            Ibici      += ddt(model->HICUMtype*(Qdci+Qjci));
                    error = NIintegrate(ckt,&geq,&ceq,Cdci,here->HICUMqdci);
                    if(error) return(error);
                    Ibici_Vbici += geq;
                    Ibici += *(ckt->CKTstate0 + here->HICUMcqdci);

                    error = NIintegrate(ckt,&geq,&ceq,Cjci,here->HICUMqjci);
                    if(error) return(error);
                    Ibici_Vbici += geq;
                    Ibici += *(ckt->CKTstate0 + here->HICUMcqjci);

//            Ibpei      += ddt(model->HICUMtype*Qjep);
                    error = NIintegrate(ckt,&geq,&ceq,Cjep,here->HICUMqjep);
                    if(error) return(error);
                    Ibpei_Vbpei += geq;
                    Ibpei += *(ckt->CKTstate0 + here->HICUMcqjep);

//            Isici      += ddt(model->HICUMtype*Qjs);
                    error = NIintegrate(ckt,&geq,&ceq,Cjs,here->HICUMqjs);
                    if(error) return(error);
                    Isici_Vsici += geq;
                    Isici += *(ckt->CKTstate0 + here->HICUMcqjs);

//            Ibci       += ddt(model->HICUMtype*qjcx0_t_i);
                    // error = NIintegrate(ckt,&geq,&ceq,Cjcx_i,here->HICUMqjcx0_i);
                    // if(error) return(error);
                    // Ibci_Vbci = geq;
                    // Ibci = *(ckt->CKTstate0 + here->HICUMcqcx0_t_i);

//            Ibpci      += ddt(model->HICUMtype*(qjcx0_t_ii+Qdsu));
                    error = NIintegrate(ckt,&geq,&ceq,Cjcx_ii,here->HICUMqjcx0_ii);
                    if(error) return(error);
                    Ibpci_Vbpci += geq;
                    Ibpci += *(ckt->CKTstate0 + here->HICUMcqcx0_t_ii);

                    error = NIintegrate(ckt,&geq,&ceq,Qdsu_Vbpci,here->HICUMqdsu);
                    if(error) return(error);
                    Ibpci_Vbpci += geq;
                    Ibpci += *(ckt->CKTstate0 + here->HICUMcqdsu);

//            Isc        += ddt(model->HICUMtype*Qscp);
                    error = NIintegrate(ckt,&geq,&ceq,Cscp,here->HICUMqscp);
                    if(error) return(error);
                    Isc_Vsc = geq;
                    Isc = *(ckt->CKTstate0 + here->HICUMcqscp);
//NQS
//            Iqxf1 <+ ddt(Qxf1);
                    error = NIintegrate(ckt,&geq,&ceq,Qxf1_Vxf1,here->HICUMqxf);
                    if(error) return(error);
                    Iqxf1_Vxf1 = geq;
                    Iqxf1 = *(ckt->CKTstate0 + here->HICUMcqxf1);
//            Iqxf2 <+ ddt(Qxf2);
                    error = NIintegrate(ckt,&geq,&ceq,Qxf2_Vxf2,here->HICUMqxf);
                    if(error) return(error);
                    Iqxf2_Vxf2 = geq;
                    Iqxf2 = *(ckt->CKTstate0 + here->HICUMcqxf2);
//            Iqxf +=  ddt(Qxf);    //for RC nw
                    error = NIintegrate(ckt,&geq,&ceq,Qxf_Vxf,here->HICUMqxf);
                    if(error) return(error);
                    Iqxf_Vxf = geq;
                    Iqxf = *(ckt->CKTstate0 + here->HICUMcqxf);

                    if (model->HICUMflsh)
                    {
//              Ith       += ddt(model->HICUMcth*Vrth);
                        error = NIintegrate(ckt,&geq,&ceq,model->HICUMcth,here->HICUMqcth);
                        if(error) return(error);
                        Icth_Vrth = geq;
                        Icth = *(ckt->CKTstate0 + here->HICUMcqcth);
                    }

                    if(ckt->CKTmode & MODEINITTRAN) {
                        *(ckt->CKTstate1 + here->HICUMcqrbi) =
                                *(ckt->CKTstate0 + here->HICUMcqrbi);
                        *(ckt->CKTstate1 + here->HICUMcqjei) =
                                *(ckt->CKTstate0 + here->HICUMcqjei);
                        *(ckt->CKTstate1 + here->HICUMcqdeix) =
                                *(ckt->CKTstate0 + here->HICUMcqdeix);
                        *(ckt->CKTstate1 + here->HICUMcqjci) =
                                *(ckt->CKTstate0 + here->HICUMcqjci);
                        *(ckt->CKTstate1 + here->HICUMcqdci) =
                                *(ckt->CKTstate0 + here->HICUMcqdci);
                        *(ckt->CKTstate1 + here->HICUMcqjep) =
                                *(ckt->CKTstate0 + here->HICUMcqjep);
                        *(ckt->CKTstate1 + here->HICUMcqcx0_t_i) =
                                *(ckt->CKTstate0 + here->HICUMcqcx0_t_i);
                        *(ckt->CKTstate1 + here->HICUMcqcx0_t_ii) =
                                *(ckt->CKTstate0 + here->HICUMcqcx0_t_ii);
                        *(ckt->CKTstate1 + here->HICUMcqdsu) =
                                *(ckt->CKTstate0 + here->HICUMcqdsu);
                        *(ckt->CKTstate1 + here->HICUMcqjs) =
                                *(ckt->CKTstate0 + here->HICUMcqjs);
                        *(ckt->CKTstate1 + here->HICUMcqscp) =
                                *(ckt->CKTstate0 + here->HICUMcqscp);
                        if (model->HICUMflsh)
                            *(ckt->CKTstate1 + here->HICUMcqcth) =
                                    *(ckt->CKTstate0 + here->HICUMcqcth);
                    }
                }
            }

            /*
             *   check convergence
             */
            if ( (!(ckt->CKTmode & MODEINITFIX))||(!(here->HICUMoff))) {
                if (icheck == 1) {
                    ckt->CKTnoncon++;
                    ckt->CKTtroubleElt = (GENinstance *) here;
                }
            }

            /*
             *      charge storage for outer junctions
             */
            if(ckt->CKTmode & (MODETRAN | MODEAC)) {
//            Ibe        += ddt(cbepar1*Vbe);
                error = NIintegrate(ckt,&gqbepar1,&cqbepar1,cbepar1,here->HICUMqbepar1);
                if(error) return(error);
//            Ibpe       += ddt(cbepar2*Vbpe);
                error = NIintegrate(ckt,&gqbepar2,&cqbepar2,cbepar2,here->HICUMqbepar2);
                if(error) return(error);
//            Ibci       += ddt(cbcpar1*Vbci);
                error = NIintegrate(ckt,&gqbcpar1,&cqbcpar1,cbcpar1,here->HICUMqbcpar1);
                if(error) return(error);
//            Ibpci      += ddt(cbcpar2*Vbpci);
                error = NIintegrate(ckt,&gqbcpar2,&cqbcpar2,cbcpar2,here->HICUMqbcpar2);
                if(error) return(error);
//            Isis      += ddt(model->HICUMcsu*Vsis);
                error = NIintegrate(ckt,&gqsu,&cqsu,model->HICUMcsu,here->HICUMqsu);
                if(error) return(error);
//tocheck: Currents and conductances must used in stamping below
                if(ckt->CKTmode & MODEINITTRAN) {
                    *(ckt->CKTstate1 + here->HICUMcqbepar1) =
                            *(ckt->CKTstate0 + here->HICUMcqbepar1);
                    *(ckt->CKTstate1 + here->HICUMcqbepar2) =
                            *(ckt->CKTstate0 + here->HICUMcqbepar2);
                    *(ckt->CKTstate1 + here->HICUMcqbcpar1) =
                            *(ckt->CKTstate0 + here->HICUMcqbcpar1);
                    *(ckt->CKTstate1 + here->HICUMcqbcpar2) =
                            *(ckt->CKTstate0 + here->HICUMcqbcpar2);
                    *(ckt->CKTstate1 + here->HICUMcqsu) =
                            *(ckt->CKTstate0 + here->HICUMcqsu);
                }
            }

            *(ckt->CKTstate0 + here->HICUMvbiei)       = Vbiei;
            *(ckt->CKTstate0 + here->HICUMvbici)       = Vbici;
            *(ckt->CKTstate0 + here->HICUMvbpei)       = Vbpei;
            *(ckt->CKTstate0 + here->HICUMvbpbi)       = Vbpbi;
            *(ckt->CKTstate0 + here->HICUMvbpci)       = Vbpci;
            *(ckt->CKTstate0 + here->HICUMvsici)       = Vsici;

            *(ckt->CKTstate0 + here->HICUMibiei)       = Ibiei;
            *(ckt->CKTstate0 + here->HICUMibiei_Vbiei) = Ibiei_Vbiei;

            *(ckt->CKTstate0 + here->HICUMibpei)       = Ibpei;
            *(ckt->CKTstate0 + here->HICUMibpei_Vbpei) = Ibpei_Vbpei;

            *(ckt->CKTstate0 + here->HICUMiciei)       = Iciei;
            *(ckt->CKTstate0 + here->HICUMiciei_Vbiei) = Iciei_Vbiei;
            *(ckt->CKTstate0 + here->HICUMiciei_Vbici) = Iciei_Vbici;

            *(ckt->CKTstate0 + here->HICUMibici)       = Ibici;
            *(ckt->CKTstate0 + here->HICUMibici_Vbici) = Ibici_Vbici;

            *(ckt->CKTstate0 + here->HICUMibpbi)       = Ibpbi;
            *(ckt->CKTstate0 + here->HICUMibpbi_Vbpbi) = Ibpbi_Vbpbi;
            *(ckt->CKTstate0 + here->HICUMibpbi_Vbiei) = Ibpbi_Vbiei;
            *(ckt->CKTstate0 + here->HICUMibpbi_Vbici) = Ibpbi_Vbici;

            *(ckt->CKTstate0 + here->HICUMibpci)       = Ibpci;
            *(ckt->CKTstate0 + here->HICUMibpci_Vbpci) = Ibpci_Vbpci;

            *(ckt->CKTstate0 + here->HICUMisici)       = Isici;
            *(ckt->CKTstate0 + here->HICUMisici_Vsici) = Isici_Vsici;

            *(ckt->CKTstate0 + here->HICUMibpsi)       = Ibpsi;
            *(ckt->CKTstate0 + here->HICUMibpsi_Vbpci) = Ibpsi_Vbpci;
            *(ckt->CKTstate0 + here->HICUMibpsi_Vsici) = Ibpsi_Vsici;

            *(ckt->CKTstate0 + here->HICUMisis_Vsis)   = Isis_Vsis;

            *(ckt->CKTstate0 + here->HICUMieie)        = Ieie;

            *(ckt->CKTstate0 + here->HICUMcqcth)       = Icth;
            *(ckt->CKTstate0 + here->HICUMicth_Vrth)   = Icth_Vrth;

            *(ckt->CKTstate0 + here->HICUMgqbepar1)    = gqbepar1;
            *(ckt->CKTstate0 + here->HICUMgqbepar2)    = gqbepar2;

            *(ckt->CKTstate0 + here->HICUMgqbcpar1)    = gqbcpar1;
            *(ckt->CKTstate0 + here->HICUMgqbcpar2)    = gqbcpar2;

            *(ckt->CKTstate0 + here->HICUMgqsu)        = gqsu;

load:
            /*
             *  load current excitation vector and matrix
             */
/*
c           Branch: be, Stamp element: Cbepar1
*/
            rhs_current = model->HICUMtype * (*(ckt->CKTstate0 + here->HICUMcqbepar1) - Vbe * gqbepar1);
            *(ckt->CKTrhs + here->HICUMbaseNode)   += -rhs_current;
            *(here->HICUMbaseBasePtr)              +=  gqbepar1;
            *(here->HICUMemitEmitPtr)              +=  gqbepar1;
            *(ckt->CKTrhs + here->HICUMemitNode)   +=  rhs_current;
            *(here->HICUMbaseEmitPtr)              += -gqbepar1;
            *(here->HICUMemitBasePtr)              += -gqbepar1;
/*
c           Branch: bpe, Stamp element: Cbepar2
*/
            rhs_current = model->HICUMtype * (*(ckt->CKTstate0 + here->HICUMcqbepar2) - Vbpe * gqbepar2);
            *(ckt->CKTrhs + here->HICUMbaseBPNode) += -rhs_current;
            *(here->HICUMbaseBPBaseBPPtr)          +=  gqbepar2;
            *(here->HICUMbaseBPEmitPtr)            +=  gqbepar2;
            *(ckt->CKTrhs + here->HICUMemitNode)   +=  rhs_current;
            *(here->HICUMemitBaseBPPtr)            += -gqbepar2;
            *(here->HICUMemitEmitPtr)              += -gqbepar2;
/*
c           Branch: bci, Stamp element: Cbcpar1
*/
            rhs_current = model->HICUMtype * (*(ckt->CKTstate0 + here->HICUMcqbcpar1) - Vbci * gqbcpar1);
            *(ckt->CKTrhs + here->HICUMbaseNode)   += -rhs_current;
            *(here->HICUMbaseBasePtr)              +=  gqbcpar1;
            *(here->HICUMcollCICollCIPtr)          +=  gqbcpar1;
            *(ckt->CKTrhs + here->HICUMcollCINode) +=  rhs_current;
            *(here->HICUMbaseCollCIPtr)            += -gqbcpar1;
            *(here->HICUMcollCIBasePtr)            += -gqbcpar1;
/*
c           Branch: bpci, Stamp element: Cbcpar2
*/
            rhs_current = model->HICUMtype * (*(ckt->CKTstate0 + here->HICUMcqbcpar2) - Vbpci * gqbcpar2);
            *(ckt->CKTrhs + here->HICUMbaseBPNode) += -rhs_current;
            *(here->HICUMbaseBPBaseBPPtr)          +=  gqbcpar2;
            *(here->HICUMcollCICollCIPtr)          +=  gqbcpar2;
            *(ckt->CKTrhs + here->HICUMcollCINode) +=  rhs_current;
            *(here->HICUMbaseBPCollCIPtr)          += -gqbcpar2;
            *(here->HICUMcollCIBaseBPPtr)          += -gqbcpar2;
/*
c           Branch: ssi, Stamp element: Csu
*/
            rhs_current = model->HICUMtype * (*(ckt->CKTstate0 + here->HICUMcqsu) - Vsis * gqsu);
            *(ckt->CKTrhs + here->HICUMsubsNode)   += -rhs_current;
            *(here->HICUMsubsSubsPtr)              +=  gqsu;
            *(here->HICUMsubsSISubsSIPtr)          +=  gqsu;
            *(ckt->CKTrhs + here->HICUMsubsSINode) +=  rhs_current;
            *(here->HICUMsubsSubsSIPtr)            += -gqsu;
            *(here->HICUMsubsSISubsPtr)            += -gqsu;
/*
c           Branch: biei, Stamp element: Ijbei
*/
            rhs_current = model->HICUMtype * (Ibiei - Ibiei_Vbiei*Vbiei);
            *(ckt->CKTrhs + here->HICUMbaseBINode) += -rhs_current;
            *(here->HICUMbaseBIBaseBIPtr)          +=  Ibiei_Vbiei;
            *(here->HICUMbaseBIEmitEIPtr)          += -Ibiei_Vbiei;
            *(ckt->CKTrhs + here->HICUMemitEINode) +=  rhs_current;
            *(here->HICUMemitEIBaseBIPtr)          += -Ibiei_Vbiei;
            *(here->HICUMemitEIEmitEIPtr)          +=  Ibiei_Vbiei;
/*
c           Branch: bpei, Stamp element: Ijbep
*/
            rhs_current = model->HICUMtype * (Ibpei - Ibpei_Vbpei*Vbpei);
            *(ckt->CKTrhs + here->HICUMbaseBPNode) += -rhs_current;
            *(here->HICUMbaseBPBaseBPPtr)          +=  Ibpei_Vbpei;
            *(here->HICUMbaseBPEmitEIPtr)          += -Ibpei_Vbpei;
            *(ckt->CKTrhs + here->HICUMemitEINode) +=  rhs_current;
            *(here->HICUMemitEIBaseBPPtr)          += -Ibpei_Vbpei;
            *(here->HICUMemitEIEmitEIPtr)          +=  Ibpei_Vbpei;

/*
c           Branch: bici, Stamp element: Ijbci
*/
            rhs_current = model->HICUMtype * (Ibici - Ibici_Vbici*Vbici);
            *(ckt->CKTrhs + here->HICUMbaseBINode) += -rhs_current;
            *(here->HICUMbaseBIBaseBIPtr)          +=  Ibici_Vbici;
            *(here->HICUMbaseBICollCIPtr)          += -Ibici_Vbici;
            *(ckt->CKTrhs + here->HICUMcollCINode) +=  rhs_current;
            *(here->HICUMcollCIBaseBIPtr)          += -Ibici_Vbici;
            *(here->HICUMcollCICollCIPtr)          +=  Ibici_Vbici;
/*
c           Branch: ciei, Stamp element: It
*/
            rhs_current = model->HICUMtype * (Iciei - Iciei_Vbiei*Vbiei - Iciei_Vbici*Vbici);
            *(ckt->CKTrhs + here->HICUMcollCINode) += -rhs_current;
            *(here->HICUMcollCIBaseBIPtr)          +=  Iciei_Vbiei;
            *(here->HICUMcollCIEmitEIPtr)          += -Iciei_Vbiei;
            *(here->HICUMcollCIBaseBIPtr)          +=  Iciei_Vbici;
            *(here->HICUMcollCICollCIPtr)          += -Iciei_Vbici;
            *(ckt->CKTrhs + here->HICUMemitEINode) +=  rhs_current;
            *(here->HICUMemitEIBaseBIPtr)          += -Iciei_Vbiei;
            *(here->HICUMemitEIEmitEIPtr)          +=  Iciei_Vbiei;
            *(here->HICUMemitEIBaseBIPtr)          += -Iciei_Vbici;
            *(here->HICUMemitEICollCIPtr)          +=  Iciei_Vbici;
/*
c           Branch: bci, Stamp element: Qbcx
*/
            rhs_current = model->HICUMtype * (ibci - ibci_Vbci*Vbci);
            *(ckt->CKTrhs + here->HICUMbaseNode)   += -rhs_current;
            *(here->HICUMbaseBasePtr)              +=  ibci_Vbci;
            *(here->HICUMbaseCollCIPtr)            += -ibci_Vbci;
            *(ckt->CKTrhs + here->HICUMcollCINode) +=  rhs_current;
            *(here->HICUMcollCIBasePtr)            += -ibci_Vbci;
            *(here->HICUMcollCICollCIPtr)          +=  ibci_Vbci;
/*
c           Branch: bpci, Stamp element: Ijbcx
*/
            rhs_current = model->HICUMtype * (Ibpci - Ibpci_Vbpci*Vbpci);
            *(ckt->CKTrhs + here->HICUMbaseBPNode) += -rhs_current;
            *(here->HICUMbaseBPCollCIPtr)          +=  Ibpci_Vbpci;
            *(here->HICUMbaseBPBaseBPPtr)          += -Ibpci_Vbpci;
            *(ckt->CKTrhs + here->HICUMcollCINode) +=  rhs_current;
            *(here->HICUMcollCIBaseBPPtr)          += -Ibpci_Vbpci;
            *(here->HICUMcollCICollCIPtr)          +=  Ibpci_Vbpci;
/*
c           Branch: cic, Stamp element: Rcx
*/
            *(here->HICUMcollCollPtr)     +=  Icic_Vcic;
            *(here->HICUMcollCICollCIPtr) +=  Icic_Vcic;
            *(here->HICUMcollCICollPtr)   += -Icic_Vcic;
            *(here->HICUMcollCollCIPtr)   += -Icic_Vcic;

/*
c           Branch: bbp, Stamp element: Rbx
*/
            *(here->HICUMbaseBasePtr)     +=  Ibbp_Vbbp;
            *(here->HICUMbaseBPBaseBPPtr) +=  Ibbp_Vbbp;
            *(here->HICUMbaseBPBasePtr)   += -Ibbp_Vbbp;
            *(here->HICUMbaseBaseBPPtr)   += -Ibbp_Vbbp;
/*
c           Branch: eie, Stamp element: Re
*/
            *(here->HICUMemitEmitPtr)     +=  Ieie_Veie;
            *(here->HICUMemitEIEmitEIPtr) +=  Ieie_Veie;
            *(here->HICUMemitEIEmitPtr)   += -Ieie_Veie;
            *(here->HICUMemitEmitEIPtr)   += -Ieie_Veie;
/*
c           Branch: bpbi, Stamp element: Rbi, Crbi
*/
            rhs_current = model->HICUMtype * (Ibpbi - Ibpbi_Vbpbi*Vbpbi - Ibpbi_Vbiei*Vbiei - Ibpbi_Vbici*Vbici);
//printf("Ibpbi_Vbpbi: %g Vbpbi: %f Ibpbi_Vbiei: %g Vbiei: %f Ibpbi_Vbici: %g Vbici: %f\n", Ibpbi_Vbpbi, Vbpbi, Ibpbi_Vbiei, Vbiei, Ibpbi_Vbici, Vbici);
//printf("Ibpbi: %g RHS: %g\n", Ibpbi, rhs_current);
            *(ckt->CKTrhs + here->HICUMbaseBPNode) += -rhs_current;
            *(here->HICUMbaseBPBaseBPPtr)          +=  Ibpbi_Vbpbi;
            *(here->HICUMbaseBPBaseBIPtr)          += -Ibpbi_Vbpbi;
            *(here->HICUMbaseBPBaseBIPtr)          +=  Ibpbi_Vbiei;
            *(here->HICUMbaseBPEmitEIPtr)          += -Ibpbi_Vbiei;
            *(here->HICUMbaseBPCollCIPtr)          +=  Ibpbi_Vbici;
            *(here->HICUMbaseBPEmitEIPtr)          += -Ibpbi_Vbici;
            *(ckt->CKTrhs + here->HICUMbaseBINode) +=  rhs_current;
            *(here->HICUMbaseBIBaseBPPtr)          += -Ibpbi_Vbpbi;
            *(here->HICUMbaseBIBaseBIPtr)          +=  Ibpbi_Vbpbi;
            *(here->HICUMbaseBIBaseBIPtr)          += -Ibpbi_Vbiei;
            *(here->HICUMbaseBIEmitEIPtr)          +=  Ibpbi_Vbiei;
            *(here->HICUMbaseBICollCIPtr)          += -Ibpbi_Vbici;
            *(here->HICUMbaseBIEmitEIPtr)          +=  Ibpbi_Vbici;
/*
c           Branch: sc, Stamp element: Cscp
*/
            rhs_current = model->HICUMtype * (Isc - Isc_Vsc*Vsc);
            *(ckt->CKTrhs + here->HICUMsubsNode) += -rhs_current;
            *(here->HICUMsubsSubsPtr)            +=  Isc_Vsc;
            *(here->HICUMsubsCollPtr)            +=  Isc_Vsc;
            *(ckt->CKTrhs + here->HICUMcollNode) +=  rhs_current;
            *(here->HICUMcollSubsPtr)            += -Isc_Vsc;
            *(here->HICUMcollCollPtr)            += -Isc_Vsc;
/*
c           Branch: sici, Stamp element: Ijsc
*/
            rhs_current = model->HICUMtype * (Isici - Isici_Vsici*Vsici);
            *(ckt->CKTrhs + here->HICUMsubsSINode) += -rhs_current;
            *(here->HICUMsubsSISubsSIPtr)          +=  Isici_Vsici;
            *(here->HICUMsubsSICollCIPtr)          += -Isici_Vsici;
            *(ckt->CKTrhs + here->HICUMcollCINode) +=  rhs_current;
            *(here->HICUMcollCISubsSIPtr)          += -Isici_Vsici;
            *(here->HICUMcollCICollCIPtr)          +=  Isici_Vsici;
/*
c           Branch: bpsi, Stamp element: Its
*/
            rhs_current = model->HICUMtype * (Ibpsi - Ibpsi_Vbpci*Vbpci - Ibpsi_Vsici*Vsici);
            *(ckt->CKTrhs + here->HICUMbaseBPNode) += -rhs_current;
            *(here->HICUMbaseBPBaseBPPtr)          +=  Ibpsi_Vbpci;
            *(here->HICUMbaseBPCollCIPtr)          += -Ibpsi_Vbpci;
            *(here->HICUMbaseBPSubsSIPtr)          +=  Ibpsi_Vsici;
            *(here->HICUMbaseBPCollCIPtr)          += -Ibpsi_Vsici;
            *(ckt->CKTrhs + here->HICUMsubsSINode) +=  rhs_current;
            *(here->HICUMsubsSIBaseBPPtr)          += -Ibpsi_Vbpci;
            *(here->HICUMsubsSICollCIPtr)          +=  Ibpsi_Vbpci;
            *(here->HICUMsubsSISubsSIPtr)          += -Ibpsi_Vsici;
            *(here->HICUMsubsSICollCIPtr)          +=  Ibpsi_Vsici;
/*
c           Branch: sis, Stamp element: Rsu
*/
            *(here->HICUMsubsSubsPtr)     +=  Isis_Vsis;
            *(here->HICUMsubsSISubsSIPtr) +=  Isis_Vsis;
            *(here->HICUMsubsSISubsPtr)   += -Isis_Vsis;
            *(here->HICUMsubsSubsSIPtr)   += -Isis_Vsis;
//NQS
/*
c           Branch: xf1-ground,  Stamp element: Ixf1
*/
//            rhs_current = (Ixf1 - Ixf1_Vxf1*Vxf1 - Ixf1_Vrth*Vrth - Ixf1_Vbiei*Vbiei - Ixf1_Vbici*Vbici - Ixf1_Vxf2*Vxf2); // TODO
            rhs_current = Ixf1;
            *(ckt->CKTrhs + here->HICUMxf1Node) += 0; // rhs_current; // into xf1 node
//            *(here->HICUMxf1TempPtr)   += -Ixf1_Vrth;
//            *(here->HICUMxf1BaseBIPtr) += -Ixf1_Vbiei;
//            *(here->HICUMxf1EmitEIPtr) += +Ixf1_Vbiei;
//            *(here->HICUMxf1BaseBIPtr) += -Ixf1_Vbici;
//            *(here->HICUMxf1CollCIPtr) += +Ixf1_Vbici;
//            *(here->HICUMxf1Xf2Ptr)    += +Ixf1_Vxf2;
/*
c           Branch: xf1-ground, Stamp element: Qxf1 // TODO Test in AC simulation!
*/
            // rhs_current = Iqxf1 - Iqxf1_Vxf1*Vxf1;
            // *(ckt->CKTrhs + here->HICUMxf1Node) += rhs_current; // into ground
            // *(here->HICUMxf1Xf1Ptr)             += Iqxf1_Vxf1;
/*
c           Branch: xf1-ground, Stamp element: Rxf1 TODO: This is wrong, but needed at the moment for convergence
*/
            *(here->HICUMxf1Xf1Ptr) +=  1; // current Ixf1 is normalized to Tf
/*
c           Branch: xf2-ground,  Stamp element: Ixf2
*/
//            rhs_current = (Ixf2 - Ixf2_Vxf1*Vxf1 - Ixf2_Vrth*Vrth - Ixf2_Vbiei*Vbiei - Ixf2_Vbici*Vbici - Ixf2_Vxf2*Vxf2); // TODO
            rhs_current = Ixf2;
            *(ckt->CKTrhs + here->HICUMxf2Node) += rhs_current; // into xf2 node
//            *(here->HICUMxf2TempPtr)   += -Ixf2_Vrth;
//            *(here->HICUMxf2BaseBIPtr) += -Ixf2_Vbiei;
//            *(here->HICUMxf2EmitEIPtr) += +Ixf2_Vbiei;
//            *(here->HICUMxf2BaseBIPtr) += -Ixf2_Vbici;
//            *(here->HICUMxf2CollCIPtr) += +Ixf2_Vbici;
/*
c           Branch: xf2-ground, Stamp element: Qxf2 // TODO Test in AC simulation!
*/
            // rhs_current = Iqxf2 - Iqxf2_Vxf2*Vxf2;
            // *(ckt->CKTrhs + here->HICUMxf2Node)  += rhs_current; // into ground
            // *(here->HICUMxf2Xf2Ptr)              += Iqxf2_Vxf2;
/*
c           Branch: xf2-ground, Stamp element: Rxf2
*/
            *(here->HICUMxf2Xf2Ptr) +=  1; // current Ixf2 is normalized to Tf
/*
c           Branch: xf-ground,  Stamp element: Ixf
*/
//            rhs_current = model->HICUMtype * (Ixf - Ixf_Vrth*Vrth - Ixf_Vbiei*Vbiei - Ixf_Vbici*Vbici);
            rhs_current = model->HICUMtype * Ixf;
            *(ckt->CKTrhs + here->HICUMxfNode) += rhs_current; // into xf node
//            *(here->HICUMxfTempPtr)   += -Ixf_Vrth;
//            *(here->HICUMxfBaseBIPtr) += -Ixf_Vbiei;
//            *(here->HICUMxfEmitEIPtr) += +Ixf_Vbiei;
//            *(here->HICUMxfBaseBIPtr) += -Ixf_Vbici;
//            *(here->HICUMxfCollCIPtr) += +Ixf_Vbici;
/*
c           Branch: xf-ground, Stamp element: Qxf // TODO Test in AC simulation!
*/
            // rhs_current = model->HICUMtype * (Iqxf - Iqxf_Vxf*Vxf);
            // *(ckt->CKTrhs + here->HICUMxfNode)   += rhs_current; // into ground
            // *(here->HICUMxfXfPtr)                += Iqxf_Vxf;
/*
c           Branch: xf-ground, Stamp element: Rxf
*/
            *(here->HICUMxfXfPtr) +=  1; // current Ixf is normalized to Tf

            if (model->HICUMflsh) {
/*
c               Stamp element: Ibiei
*/
                rhs_current = -Ibiei_dT*Vrth;
                *(ckt->CKTrhs + here->HICUMbaseBINode) += -rhs_current;
                *(here->HICUMbaseBItempPtr)            +=  Ibiei_dT;
                *(ckt->CKTrhs + here->HICUMemitEINode) +=  rhs_current;
                *(here->HICUMemitEItempPtr)            += -Ibiei_dT;
/*
c               Stamp element: Ibici
*/
                rhs_current = -Ibici_Vrth*Vrth;
                *(ckt->CKTrhs + here->HICUMbaseBINode) += -rhs_current;
                *(here->HICUMbaseBItempPtr)            +=  Ibici_Vrth;
                *(ckt->CKTrhs + here->HICUMcollCINode) +=  rhs_current;
                *(here->HICUMcollCItempPtr)            += -Ibici_Vrth;
/*
c               Stamp element: Iciei
*/
                rhs_current = -Iciei_Vrth*Vrth;
                *(ckt->CKTrhs + here->HICUMcollCINode) += -rhs_current;
                *(here->HICUMcollCItempPtr)            +=  Iciei_Vrth;
                *(ckt->CKTrhs + here->HICUMemitEINode) +=  rhs_current;
                *(here->HICUMemitEItempPtr)            += -Iciei_Vrth;
/*
c               Stamp element: Ibpei
*/
                rhs_current = -Ibpei_Vrth*Vrth;
                *(ckt->CKTrhs + here->HICUMbaseBPNode) += -rhs_current;
                *(here->HICUMbaseBPtempPtr)            +=  Ibpei_Vrth;
                *(ckt->CKTrhs + here->HICUMemitEINode) +=  rhs_current;
                *(here->HICUMemitEItempPtr)            += -Ibpei_Vrth;
/*
c               Stamp element: Ibpci
*/
                rhs_current = -Ibpci_Vrth*Vrth;
                *(ckt->CKTrhs + here->HICUMbaseBPNode) += -rhs_current;
                *(here->HICUMbaseBPtempPtr)            +=  Ibpci_Vrth;
                *(ckt->CKTrhs + here->HICUMcollCINode) +=  rhs_current;
                *(here->HICUMcollCItempPtr)            += -Ibpci_Vrth;
/*
c               Stamp element: Isici
*/
                rhs_current = -Isici_Vrth*Vrth;
                *(ckt->CKTrhs + here->HICUMsubsSINode) += -rhs_current;
                *(here->HICUMsubsSItempPtr)            +=  Isici_Vrth;
                *(ckt->CKTrhs + here->HICUMcollCINode) +=  rhs_current;
                *(here->HICUMcollCItempPtr)            += -Isici_Vrth;
/*
c               Stamp element: Rbi
*/
                rhs_current = -Ibpbi_Vrth*Vrth;
                *(ckt->CKTrhs + here->HICUMbaseBPNode) += -rhs_current;
                *(here->HICUMbaseBPtempPtr)            +=  Ibpbi_Vrth;
                *(ckt->CKTrhs + here->HICUMbaseBINode) +=  rhs_current;
                *(here->HICUMbaseBItempPtr)            += -Ibpbi_Vrth;
/*
c               Stamp element: Isici
*/
                rhs_current = -Isici_Vrth*Vrth;
                *(ckt->CKTrhs + here->HICUMsubsSINode) += -rhs_current;
                *(here->HICUMsubsSItempPtr)            +=  Isici_Vrth;
                *(ckt->CKTrhs + here->HICUMcollCINode) +=  rhs_current;
                *(here->HICUMcollCItempPtr)            += -Isici_Vrth;
/*
c               Stamp element: Rcx
*/
                *(here->HICUMcollTempPtr) +=  Icic_Vrth;
/*
c               Stamp element: Rbx
*/
                *(here->HICUMbaseTempPtr) +=  Ibbp_Vrth;
/*
c               Stamp element: Re
*/
                *(here->HICUMemitTempPtr) +=  Ieie_Vrth;
/*
c               Stamp element: Rth
*/
                *(here->HICUMtempTempPtr) +=  Irth_Vrth;
/*
c               Stamp element: Cth
*/
                *(here->HICUMtempTempPtr) +=  Icth_Vrth;
/*
c               Stamp element: Ith
*/
                rhs_current = Ith + Icth - Icth_Vrth*Vrth
                              + Ith_Vbiei*Vbiei + Ith_Vbici*Vbici + Ith_Vciei*Vciei
                              + Ith_Vbpei*Vbpei + Ith_Vbpci*Vbpci + Ith_Vsici*Vsici
                              + Ith_Vbpbi*Vbpbi
                              + Ith_Vcic*Vcic + Ith_Vbbp*Vbbp + Ith_Veie*Veie;

                *(ckt->CKTrhs + here->HICUMtempNode) -= rhs_current;

                *(here->HICUMtempTempPtr)   += -Ith_Vrth;

                *(here->HICUMtempBaseBIPtr) += -Ith_Vbiei;
                *(here->HICUMtempEmitEIPtr) += +Ith_Vbiei;

                *(here->HICUMtempBaseBIPtr) += -Ith_Vbici;
                *(here->HICUMtempCollCIPtr) += +Ith_Vbici;

                *(here->HICUMtempCollCIPtr) += -Ith_Vciei;
                *(here->HICUMtempEmitEIPtr) += +Ith_Vciei;

                *(here->HICUMtempBaseBPPtr) += -Ith_Vbpei;
                *(here->HICUMtempEmitEIPtr) += +Ith_Vbpei;

                *(here->HICUMtempBaseBPPtr) += -Ith_Vbpci;
                *(here->HICUMtempCollCIPtr) += +Ith_Vbpci;

                *(here->HICUMtempSubsSIPtr) += -Ith_Vsici;
                *(here->HICUMtempCollCIPtr) += +Ith_Vsici;

                *(here->HICUMtempBaseBPPtr) += -Ith_Vbpbi;
                *(here->HICUMtempBaseBIPtr) += +Ith_Vbpbi;

                *(here->HICUMtempCollCIPtr) += +Ith_Vcic;
                *(here->HICUMtempBaseBPPtr) += +Ith_Vbbp;
                *(here->HICUMtempEmitEIPtr) += +Ith_Veie;
            }
        }

    }
    return(OK);
}

/* HICUMlimitlog(deltemp, deltemp_old, LIM_TOL, check)
 * Logarithmic damping the per-iteration change of deltemp beyond LIM_TOL.
 */
static double
HICUMlimitlog(
    double deltemp,
    double deltemp_old,
    double LIM_TOL,
    int *check)
{
    *check = 0;
    if (isnan (deltemp) || isnan (deltemp_old))
    {
        fprintf(stderr, "Alberto says:  YOU TURKEY!  The limiting function received NaN.\n");
        fprintf(stderr, "New prediction returns to 0.0!\n");
        deltemp = 0.0;
        *check = 1;
    }
    /* Logarithmic damping of deltemp beyond LIM_TOL */
    if (deltemp > deltemp_old + LIM_TOL) {
        deltemp = deltemp_old + LIM_TOL + log10((deltemp-deltemp_old)/LIM_TOL);
        *check = 1;
    }
    else if (deltemp < deltemp_old - LIM_TOL) {
        deltemp = deltemp_old - LIM_TOL - log10((deltemp_old-deltemp)/LIM_TOL);
        *check = 1;
    }
    return deltemp;
}