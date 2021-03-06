// @(#)root/mathcore:$Id$
// Author: L. Moneta Tue Nov 28 10:52:47 2006

/**********************************************************************
 *                                                                    *
 * Copyright (c) 2006  LCG ROOT Math Team, CERN/PH-SFT                *
 *                                                                    *
 *                                                                    *
 **********************************************************************/

// Implementation file for class FitUtil

#include "Fit/FitUtil.h"

#include "Fit/BinData.h"
#include "Fit/UnBinData.h"

#include "Math/IFunctionfwd.h"
#include "Math/IParamFunction.h"
#include "Math/Integrator.h"
#include "Math/IntegratorMultiDim.h"
#include "Math/WrappedFunction.h"
#include "Math/OneDimFunctionAdapter.h"
#include "Math/RichardsonDerivator.h"

#include "Math/Error.h"
#include "Math/Util.h"  // for safe log(x)

#include <limits>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <numeric>
//#include <memory>

//#define DEBUG
#ifdef DEBUG
#define NSAMPLE 10
#include <iostream>
#endif

//  need to implement integral option

namespace ROOT {

   namespace Fit {

      namespace FitUtil {

         // derivative with respect of the parameter to be integrated
         template<class GradFunc = IGradModelFunction>
         struct ParamDerivFunc {
            ParamDerivFunc(const GradFunc & f) : fFunc(f), fIpar(0) {}
            void SetDerivComponent(unsigned int ipar) { fIpar = ipar; }
            double operator() (const double *x, const double *p) const {
               return fFunc.ParameterDerivative( x, p, fIpar );
            }
            unsigned int NDim() const { return fFunc.NDim(); }
            const GradFunc & fFunc;
            unsigned int fIpar;
         };

// simple gradient calculator using the 2 points rule

         class SimpleGradientCalculator {

         public:
            // construct from function and gradient dimension gdim
            // gdim = npar for parameter gradient
            // gdim = ndim for coordinate gradients
            // construct (the param values will be passed later)
            // one can choose between 2 points rule (1 extra evaluation) istrat=1
            // or two point rule (2 extra evaluation)
            // (found 2 points rule does not work correctly - minuit2FitBench fails)
            SimpleGradientCalculator(int gdim, const IModelFunction & func,double eps = 2.E-8, int istrat = 1) :
               fEps(eps),
               fPrecision(1.E-8 ), // sqrt(epsilon)
               fStrategy(istrat),
               fN(gdim ),
               fFunc(func),
               fVec(std::vector<double>(gdim) ) // this can be probably optimized
            {}

            // internal method to calculate single partial derivative
            // assume cached vector fVec is already set
            double DoParameterDerivative(const double *x, const double *p, double f0, int k) const {
               double p0 = p[k];
               double h = std::max( fEps* std::abs(p0), 8.0*fPrecision*(std::abs(p0) + fPrecision) );
               fVec[k] += h;
               double deriv = 0;
               // t.b.d : treat case of infinities
               //if (fval > - std::numeric_limits<double>::max() && fval < std::numeric_limits<double>::max() )
               double f1 = fFunc(x, &fVec.front() );
               if (fStrategy > 1) {
                  fVec[k] = p0 - h;
                  double f2 = fFunc(x, &fVec.front() );
                  deriv = 0.5 * ( f2 - f1 )/h;
               }
               else
                  deriv = ( f1 - f0 )/h;

               fVec[k] = p[k]; // restore original p value
               return deriv;
            }
            // number of dimension in x (needed when calculating the integrals)
            unsigned int NDim() const {
               return fFunc.NDim();
            }
            // number of parameters (needed for grad ccalculation)
            unsigned int NPar() const {
               return fFunc.NPar();
            }

            double ParameterDerivative(const double *x, const double *p, int ipar) const {
               // fVec are the cached parameter values
               std::copy(p, p+fN, fVec.begin());
               double f0 = fFunc(x, p);
               return DoParameterDerivative(x,p,f0,ipar);
            }

            // calculate all gradient at point (x,p) knnowing already value f0 (we gain a function eval.)
            void ParameterGradient(const double * x, const double * p, double f0, double * g) {
               // fVec are the cached parameter values
               std::copy(p, p+fN, fVec.begin());
               for (unsigned int k = 0; k < fN; ++k) {
                  g[k] = DoParameterDerivative(x,p,f0,k);
               }
            }

            // calculate gradient w.r coordinate values
            void Gradient(const double * x, const double * p, double f0, double * g) {
               // fVec are the cached coordinate values
               std::copy(x, x+fN, fVec.begin());
               for (unsigned int k = 0; k < fN; ++k) {
                  double x0 = x[k];
                  double h = std::max( fEps* std::abs(x0), 8.0*fPrecision*(std::abs(x0) + fPrecision) );
                  fVec[k] += h;
                  // t.b.d : treat case of infinities
                  //if (fval > - std::numeric_limits<double>::max() && fval < std::numeric_limits<double>::max() )
                  double f1 = fFunc( &fVec.front(), p );
                  if (fStrategy > 1) {
                     fVec[k] = x0 - h;
                     double f2 = fFunc( &fVec.front(), p  );
                     g[k] = 0.5 * ( f2 - f1 )/h;
                  }
                  else
                     g[k] = ( f1 - f0 )/h;

                  fVec[k] = x[k]; // restore original x value
               }
            }

         private:

            double fEps;
            double fPrecision;
            int fStrategy; // strategy in calculation ( =1 use 2 point rule( 1 extra func) , = 2 use r point rule)
            unsigned int fN; // gradient dimension
            const IModelFunction & fFunc;
            mutable std::vector<double> fVec; // cached coordinates (or parameter values in case of gradientpar)
         };


         // function to avoid infinities or nan
         double CorrectValue(double rval) {
            // avoid infinities or nan in  rval
            if (rval > - std::numeric_limits<double>::max() && rval < std::numeric_limits<double>::max() )
               return rval;
            else if (rval < 0)
               // case -inf
               return -std::numeric_limits<double>::max();
            else
               // case + inf or nan
               return  + std::numeric_limits<double>::max();
         }
         bool CheckValue(double & rval) {
            if (rval > - std::numeric_limits<double>::max() && rval < std::numeric_limits<double>::max() )
               return true;
            else if (rval < 0) {
               // case -inf
               rval =  -std::numeric_limits<double>::max();
               return false;
            }
            else {
               // case + inf or nan
               rval =  + std::numeric_limits<double>::max();
               return false;
            }
         }


         // calculation of the integral of the gradient functions
         // for a function providing derivative w.r.t parameters
         // x1 and x2 defines the integration interval , p the parameters
         template <class GFunc>
         void CalculateGradientIntegral(const GFunc & gfunc,
                                        const double *x1, const double * x2, const double * p, double *g) {

            // needs to calculate the integral for each partial derivative
            ParamDerivFunc<GFunc> pfunc( gfunc);
            IntegralEvaluator<ParamDerivFunc<GFunc> > igDerEval( pfunc, p, true);
            // loop on the parameters
            unsigned int npar = gfunc.NPar();
            for (unsigned int k = 0; k < npar; ++k ) {
               pfunc.SetDerivComponent(k);
               g[k] = igDerEval( x1, x2 );
            }
         }



      } // end namespace  FitUtil



//___________________________________________________________________________________________________________________________
// for chi2 functions
//___________________________________________________________________________________________________________________________

double FitUtil::EvaluateChi2(const IModelFunction & func, const BinData & data, const double * p, unsigned int &, const ROOT::Fit::ExecutionPolicy & executionPolicy, unsigned nChunks) {
   // evaluate the chi2 given a  function reference  , the data and returns the value and also in nPoints
   // the actual number of used points
   // normal chi2 using only error on values (from fitting histogram)
   // optionally the integral of function in the bin is used

   unsigned int n = data.Size();

   // set parameters of the function to cache integral value
#ifdef USE_PARAMCACHE
   (const_cast<IModelFunction &>(func)).SetParameters(p);
#endif
   // do not cache parameter values (it is not thread safe)
   //func.SetParameters(p);


   // get fit option and check case if using integral of bins
   const DataOptions & fitOpt = data.Opt();
   bool useBinIntegral = fitOpt.fIntegral && data.HasBinEdges();
   bool useBinVolume = (fitOpt.fBinVolume && data.HasBinEdges());
   bool useExpErrors = (fitOpt.fExpErrors);

#ifdef DEBUG
   std::cout << "\n\nFit data size = " << n << std::endl;
   std::cout << "evaluate chi2 using function " << &func << "  " << p << std::endl;
   std::cout << "use empty bins  " << fitOpt.fUseEmpty << std::endl;
   std::cout << "use integral    " << fitOpt.fIntegral << std::endl;
   std::cout << "use all error=1 " << fitOpt.fErrors1 << std::endl;
#endif

#ifdef USE_PARAMCACHE
   IntegralEvaluator<> igEval( func, 0, useBinIntegral);
#else
   IntegralEvaluator<> igEval( func, p, useBinIntegral);
#endif
   double maxResValue = std::numeric_limits<double>::max() /n;
   double wrefVolume = 1.0;
   if (useBinVolume) {
      if (fitOpt.fNormBinVolume) wrefVolume /= data.RefVolume();
   }

   (const_cast<IModelFunction &>(func)).SetParameters(p);

   auto mapFunction = [&](const unsigned i){

      double chi2{};
      double fval{};

      const auto x1 = data.GetCoordComponent(i, 0);
      const auto y = data.Value(i);
      auto invError = data.Error(i);

      invError = (invError!= 0.0) ? 1.0/invError :1;

      const double * x = nullptr;
      std::vector<double> xc;
      double binVolume = 1.0;
      if (useBinVolume) {
         unsigned int ndim = data.NDim();
         const double * x2 = data.BinUpEdge(i);
         xc.resize(data.NDim());
         for (unsigned int j = 0; j < ndim; ++j) {
            auto xx = *data.GetCoordComponent(i, j);
            binVolume *= std::abs(x2[j]- xx);
            xc[j] = 0.5*(x2[j]+ xx);
         }
         x = xc.data();
         // normalize the bin volume using a reference value
         binVolume *= wrefVolume;
      } else if(data.NDim() > 1) {
         xc.resize(data.NDim());
         xc[0] = *x1;
         for (unsigned int j = 1; j < data.NDim(); ++j)
            xc[j] = *data.GetCoordComponent(i, j);
         x = xc.data();
      } else {
            x = x1;
      }


      if (!useBinIntegral) {
#ifdef USE_PARAMCACHE
         fval = func ( x );
#else
         fval = func ( x, p );
#endif
      }
      else {
         // calculate integral normalized by bin volume
         // need to set function and parameters here in case loop is parallelized
         fval = igEval( x, data.BinUpEdge(i)) ;
      }
      // normalize result if requested according to bin volume
      if (useBinVolume) fval *= binVolume;

      // expected errors
      if (useExpErrors) {
         // we need first to check if a weight factor needs to be applied
         // weight = sumw2/sumw = error**2/content
         double invWeight = y * invError * invError;
        //  if (invError == 0) invWeight = (data.SumOfError2() > 0) ? data.SumOfContent()/ data.SumOfError2() : 1.0;
         // compute expected error  as f(x) / weight
         double invError2 = (fval > 0) ? invWeight / fval : 0.0;
         invError = std::sqrt(invError2);
      }

//#define DEBUG
#ifdef DEBUG
      std::cout << x[0] << "  " << y << "  " << 1./invError << " params : ";
      for (unsigned int ipar = 0; ipar < func.NPar(); ++ipar)
         std::cout << p[ipar] << "\t";
      std::cout << "\tfval = " << fval << " bin volume " << binVolume << " ref " << wrefVolume << std::endl;
#endif
//#undef DEBUG

      if (invError > 0) {

         double tmp = ( y -fval )* invError;
         double resval = tmp * tmp;


         // avoid inifinity or nan in chi2 values due to wrong function values
         if ( resval < maxResValue )
            chi2 += resval;
         else {
            //nRejected++;
            chi2 += maxResValue;
         }
      }
      return chi2;
  };

#ifdef R__USE_IMT
  auto redFunction = [](const std::vector<double> & objs){
                          return std::accumulate(objs.begin(), objs.end(), double{});
  };
#else
  (void)nChunks;
#endif

  double res{};
  if(executionPolicy == ROOT::Fit::ExecutionPolicy::kSerial){
    for (unsigned int i=0; i<n; ++i) {
      res += mapFunction(i);
    }
#ifdef R__USE_IMT
  } else if(executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
    auto chunks = nChunks !=0? nChunks: setAutomaticChunking(data.Size());
    ROOT::TThreadExecutor pool;
    res = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, n), redFunction, chunks);
#endif
//   } else if(executionPolicy == ROOT::Fit::kMultitProcess){
    // ROOT::TProcessExecutor pool;
    // res = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, n), redFunction);
  } else{
    Error("FitUtil::EvaluateChi2","Execution policy unknown. Avalaible choices:\n ROOT::Fit::ExecutionPolicy::kSerial (default)\n ROOT::Fit::ExecutionPolicy::kMultithread (requires IMT)\n");
  }

   return res;
}


//___________________________________________________________________________________________________________________________

double FitUtil::EvaluateChi2Effective(const IModelFunction & func, const BinData & data, const double * p, unsigned int & nPoints) {
   // evaluate the chi2 given a  function reference  , the data and returns the value and also in nPoints
   // the actual number of used points
   // method using the error in the coordinates
   // integral of bin does not make sense in this case

   unsigned int n = data.Size();

#ifdef DEBUG
   std::cout << "\n\nFit data size = " << n << std::endl;
   std::cout << "evaluate effective chi2 using function " << &func << "  " << p << std::endl;
#endif

   assert(data.HaveCoordErrors()  || data.HaveAsymErrors());

   double chi2 = 0;
   //int nRejected = 0;


   //func.SetParameters(p);

   unsigned int ndim = func.NDim();

   // use Richardson derivator
   ROOT::Math::RichardsonDerivator derivator;

   double maxResValue = std::numeric_limits<double>::max() /n;



   for (unsigned int i = 0; i < n; ++ i) {


      double y = 0;
      const double * x = data.GetPoint(i,y);

      double fval = func( x, p );

      double delta_y_func = y - fval;


      double ey = 0;
      const double * ex = 0;
      if (!data.HaveAsymErrors() )
         ex = data.GetPointError(i, ey);
      else {
         double eylow, eyhigh = 0;
         ex = data.GetPointError(i, eylow, eyhigh);
         if ( delta_y_func < 0)
            ey = eyhigh; // function is higher than points
         else
            ey = eylow;
      }
      double e2 = ey * ey;
      // before calculating the gradient check that all error in x are not zero
      unsigned int j = 0;
      while ( j < ndim && ex[j] == 0.)  { j++; }
      // if j is less ndim some elements are not zero
      if (j < ndim) {
         // need an adapter from a multi-dim function to a one-dimensional
         ROOT::Math::OneDimMultiFunctionAdapter<const IModelFunction &> f1D(func,x,0,p);
         // select optimal step size  (use 10--2 by default as was done in TF1:
         double kEps = 0.01;
         double kPrecision = 1.E-8;
         for (unsigned int icoord = 0; icoord < ndim; ++icoord) {
            // calculate derivative for each coordinate
            if (ex[icoord] > 0) {
               //gradCalc.Gradient(x, p, fval, &grad[0]);
               f1D.SetCoord(icoord);
               // optimal spep size (take ex[] as scale for the points and 1% of it
               double x0= x[icoord];
               double h = std::max( kEps* std::abs(ex[icoord]), 8.0*kPrecision*(std::abs(x0) + kPrecision) );
               double deriv = derivator.Derivative1(f1D, x[icoord], h);
               double edx = ex[icoord] * deriv;
               e2 += edx * edx;
#ifdef DEBUG
               std::cout << "error for coord " << icoord << " = " << ex[icoord] << " deriv " << deriv << std::endl;
#endif
            }
         }
      }
      double w2 = (e2 > 0) ? 1.0/e2 : 0;
      double resval = w2 * ( y - fval ) *  ( y - fval);

#ifdef DEBUG
      std::cout << x[0] << "  " << y << " ex " << ex[0] << " ey  " << ey << " params : ";
      for (unsigned int ipar = 0; ipar < func.NPar(); ++ipar)
         std::cout << p[ipar] << "\t";
      std::cout << "\tfval = " << fval << "\tresval = " << resval << std::endl;
#endif

      // avoid (infinity and nan ) in the chi2 sum
      // eventually add possibility of excluding some points (like singularity)
      if ( resval < maxResValue )
         chi2 += resval;
      else
         chi2 += maxResValue;
      //nRejected++;

   }

   // reset the number of fitting data points
   nPoints = n;  // no points are rejected
   //if (nRejected != 0)  nPoints = n - nRejected;

#ifdef DEBUG
   std::cout << "chi2 = " << chi2 << " n = " << nPoints  << std::endl;
#endif

   return chi2;

}


////////////////////////////////////////////////////////////////////////////////
/// evaluate the chi2 contribution (residual term) only for data with no coord-errors
/// This function is used in the specialized least square algorithms like FUMILI or L.M.
/// if we have error on the coordinates the method is not yet implemented
///  integral option is also not yet implemented
///  one can use in that case normal chi2 method

double FitUtil::EvaluateChi2Residual(const IModelFunction & func, const BinData & data, const double * p, unsigned int i, double * g) {
   if (data.GetErrorType() == BinData::kCoordError && data.Opt().fCoordErrors ) {
      MATH_ERROR_MSG("FitUtil::EvaluateChi2Residual","Error on the coordinates are not used in calculating Chi2 residual");
      return 0; // it will assert otherwise later in GetPoint
   }


   //func.SetParameters(p);

   double y, invError = 0;

   const DataOptions & fitOpt = data.Opt();
   bool useBinIntegral = fitOpt.fIntegral && data.HasBinEdges();
   bool useBinVolume = (fitOpt.fBinVolume && data.HasBinEdges());
   bool useExpErrors = (fitOpt.fExpErrors);

   const double * x1 = data.GetPoint(i,y, invError);

   IntegralEvaluator<> igEval( func, p, useBinIntegral);
   double fval = 0;
   unsigned int ndim = data.NDim();
   double binVolume = 1.0;
   const double * x2 = 0;
   if (useBinVolume || useBinIntegral) x2 = data.BinUpEdge(i);

   double * xc = 0;

   if (useBinVolume) {
      xc = new double[ndim];
      for (unsigned int j = 0; j < ndim; ++j) {
         binVolume *= std::abs( x2[j]-x1[j] );
         xc[j] = 0.5*(x2[j]+ x1[j]);
      }
      // normalize the bin volume using a reference value
      binVolume /= data.RefVolume();
   }

   const double * x = (useBinVolume) ? xc : x1;

   if (!useBinIntegral) {
      fval = func ( x, p );
   }
   else {
      // calculate integral (normalized by bin volume)
      // need to set function and parameters here in case loop is parallelized
      fval = igEval( x1, x2) ;
   }
   // normalize result if requested according to bin volume
   if (useBinVolume) fval *= binVolume;

   // expected errors
   if (useExpErrors) {
      // we need first to check if a weight factor needs to be applied
      // weight = sumw2/sumw = error**2/content
      double invWeight = y * invError * invError;
      // if (invError == 0) invWeight = (data.SumOfError2() > 0) ? data.SumOfContent()/ data.SumOfError2() : 1.0;
      // compute expected error  as f(x) / weight
      double invError2 = (fval > 0) ? invWeight / fval : 0.0;
      invError = std::sqrt(invError2);
   }


   double resval =   ( y -fval )* invError;

   // avoid infinities or nan in  resval
   resval = CorrectValue(resval);

   // estimate gradient
   if (g != 0) {

      unsigned int npar = func.NPar();
      const IGradModelFunction * gfunc = dynamic_cast<const IGradModelFunction *>( &func);

      if (gfunc != 0) {
         //case function provides gradient
         if (!useBinIntegral ) {
            gfunc->ParameterGradient(  x , p, g );
         }
         else {
            // needs to calculate the integral for each partial derivative
            CalculateGradientIntegral( *gfunc, x1, x2, p, g);
         }
      }
      else {
         SimpleGradientCalculator  gc( npar, func);
         if (!useBinIntegral )
            gc.ParameterGradient(x, p, fval, g);
         else {
            // needs to calculate the integral for each partial derivative
            CalculateGradientIntegral( gc, x1, x2, p, g);
         }
      }
      // mutiply by - 1 * weight
      for (unsigned int k = 0; k < npar; ++k) {
         g[k] *= - invError;
         if (useBinVolume) g[k] *= binVolume;
      }
   }

   if (useBinVolume) delete [] xc;

   return resval;

}

void FitUtil::EvaluateChi2Gradient(const IModelFunction & f, const BinData & data, const double * p, double * grad, unsigned int & nPoints) {
   // evaluate the gradient of the chi2 function
   // this function is used when the model function knows how to calculate the derivative and we can
   // avoid that the minimizer re-computes them
   //
   // case of chi2 effective (errors on coordinate) is not supported

   if ( data.HaveCoordErrors() ) {
      MATH_ERROR_MSG("FitUtil::EvaluateChi2Residual","Error on the coordinates are not used in calculating Chi2 gradient");            return; // it will assert otherwise later in GetPoint
   }

   unsigned int nRejected = 0;

   const IGradModelFunction * fg = dynamic_cast<const IGradModelFunction *>( &f);
   assert (fg != 0); // must be called by a gradient function

   const IGradModelFunction & func = *fg;
   unsigned int n = data.Size();


#ifdef DEBUG
   std::cout << "\n\nFit data size = " << n << std::endl;
   std::cout << "evaluate chi2 using function gradient " << &func << "  " << p << std::endl;
#endif

   const DataOptions & fitOpt = data.Opt();
   bool useBinIntegral = fitOpt.fIntegral && data.HasBinEdges();
   bool useBinVolume = (fitOpt.fBinVolume && data.HasBinEdges());

   double wrefVolume = 1.0;
   std::vector<double> xc;
   if (useBinVolume) {
      if (fitOpt.fNormBinVolume) wrefVolume /= data.RefVolume();
      xc.resize(data.NDim() );
   }

   IntegralEvaluator<> igEval( func, p, useBinIntegral);

   //int nRejected = 0;
   // set values of parameters

   unsigned int npar = func.NPar();
   //   assert (npar == NDim() );  // npar MUST be  Chi2 dimension
   std::vector<double> gradFunc( npar );
   // set all vector values to zero
   std::vector<double> g( npar);

   for (unsigned int i = 0; i < n; ++ i) {


      double y, invError = 0;
      const double * x1 = data.GetPoint(i,y, invError);

      double fval = 0;
      const double * x2 = 0;

      double binVolume = 1;
      if (useBinVolume) {
         unsigned int ndim = data.NDim();
         x2 = data.BinUpEdge(i);
         for (unsigned int j = 0; j < ndim; ++j) {
            binVolume *= std::abs( x2[j]-x1[j] );
            xc[j] = 0.5*(x2[j]+ x1[j]);
         }
         // normalize the bin volume using a reference value
         binVolume *= wrefVolume;
      }

      const double * x = (useBinVolume) ? &xc.front() : x1;

      if (!useBinIntegral ) {
         fval = func ( x, p );
         func.ParameterGradient(  x , p, &gradFunc[0] );
      }
      else {
         x2 = data.BinUpEdge(i);
         // calculate normalized integral and gradient (divided by bin volume)
         // need to set function and parameters here in case loop is parallelized
         fval = igEval( x1, x2 ) ;
         CalculateGradientIntegral( func, x1, x2, p, &gradFunc[0]);
      }
      if (useBinVolume) fval *= binVolume;

#ifdef DEBUG
      std::cout << x[0] << "  " << y << "  " << 1./invError << " params : ";
      for (unsigned int ipar = 0; ipar < npar; ++ipar)
         std::cout << p[ipar] << "\t";
      std::cout << "\tfval = " << fval << std::endl;
#endif
      if ( !CheckValue(fval) ) {
         nRejected++;
         continue;
      }

      // loop on the parameters
      unsigned int ipar = 0;
      for ( ; ipar < npar ; ++ipar) {

         // correct gradient for bin volumes
         if (useBinVolume) gradFunc[ipar] *= binVolume;

         // avoid singularity in the function (infinity and nan ) in the chi2 sum
         // eventually add possibility of excluding some points (like singularity)
         double dfval = gradFunc[ipar];
         if ( !CheckValue(dfval) ) {
               break; // exit loop on parameters
         }

         // calculate derivative point contribution
         double tmp = - 2.0 * ( y -fval )* invError * invError * gradFunc[ipar];
         g[ipar] += tmp;

      }

      if ( ipar < npar ) {
          // case loop was broken for an overflow in the gradient calculation
         nRejected++;
         continue;
      }


   }

   // correct the number of points
   nPoints = n;
   if (nRejected != 0)  {
      assert(nRejected <= n);
      nPoints = n - nRejected;
      if (nPoints < npar)  MATH_ERROR_MSG("FitUtil::EvaluateChi2Gradient","Error - too many points rejected for overflow in gradient calculation");
   }

   // copy result
   std::copy(g.begin(), g.end(), grad);

}

//______________________________________________________________________________________________________
//
//  Log Likelihood functions
//_______________________________________________________________________________________________________

// utility function used by the likelihoods

// for LogLikelihood functions

double FitUtil::EvaluatePdf(const IModelFunction & func, const UnBinData & data, const double * p, unsigned int i, double * g) {
   // evaluate the pdf contribution to the generic logl function in case of bin data
   // return actually the log of the pdf and its derivatives


   //func.SetParameters(p);


   const double * x = data.Coords(i);
   double fval = func ( x, p );
   double logPdf = ROOT::Math::Util::EvalLog(fval);
   //return
   if (g == 0) return logPdf;

   const IGradModelFunction * gfunc = dynamic_cast<const IGradModelFunction *>( &func);

   // gradient  calculation
   if (gfunc != 0) {
      //case function provides gradient
      gfunc->ParameterGradient(  x , p, g );
   }
   else {
      // estimate gradieant numerically with simple 2 point rule
      // should probably calculate gradient of log(pdf) is more stable numerically
      SimpleGradientCalculator gc(func.NPar(), func);
      gc.ParameterGradient(x, p, fval, g );
   }
   // divide gradient by function value since returning the logs
   for (unsigned int ipar = 0; ipar < func.NPar(); ++ipar) {
      g[ipar] /= fval; // this should be checked against infinities
   }

#ifdef DEBUG
   std::cout << x[i] << "\t";
   std::cout << "\tpar = [ " << func.NPar() << " ] =  ";
   for (unsigned int ipar = 0; ipar < func.NPar(); ++ipar)
      std::cout << p[ipar] << "\t";
   std::cout << "\tfval = " << fval;
   std::cout << "\tgrad = [ ";
   for (unsigned int ipar = 0; ipar < func.NPar(); ++ipar)
      std::cout << g[ipar] << "\t";
   std::cout << " ] "   << std::endl;
#endif


   return logPdf;
}

double FitUtil::EvaluateLogL(const IModelFunctionTempl<double>  & func, const UnBinData & data, const double * p,
                                   int iWeight,  bool extended, unsigned int &nPoints, const ROOT::Fit::ExecutionPolicy &executionPolicy, unsigned nChunks) {
   // evaluate the LogLikelihood

   unsigned int n = data.Size();

   //unsigned int nRejected = 0;

   // set parameters of the function to cache integral value
#ifdef USE_PARAMCACHE
   (const_cast<IModelFunctionTempl<double> &>(func)).SetParameters(p);
#endif

   // this is needed if function must be normalized
   bool normalizeFunc = false;
   double norm = 1.0;
   if (normalizeFunc) {
      // compute integral of the function
      std::vector<double> xmin(data.NDim());
      std::vector<double> xmax(data.NDim());
      IntegralEvaluator<> igEval( func, p, true);
      // compute integral in the ranges where is defined
      if (data.Range().Size() > 0 ) {
         norm = 0;
         for (unsigned int ir = 0; ir < data.Range().Size(); ++ir) {
            data.Range().GetRange(&xmin[0],&xmax[0],ir);
            norm += igEval.Integral(xmin.data(),xmax.data());
         }
      } else {
         // use (-inf +inf)
         data.Range().GetRange(&xmin[0],&xmax[0]);
         // check if funcition is zero at +- inf
         if (func(xmin.data(), p) != 0 || func(xmax.data(), p) != 0) {
            MATH_ERROR_MSG("FitUtil::EvaluateLogLikelihood","A range has not been set and the function is not zero at +/- inf");
            return 0;
         }
         norm = igEval.Integral(&xmin[0],&xmax[0]);
      }
   }

   // needed to compue effective global weight in case of extended likelihood

    auto mapFunction = [&](const unsigned i){
       double W = 0;
       double W2 = 0;
       double fval = 0;

       if(data.NDim() > 1) {
          std::vector<double> x(data.NDim());
          for (unsigned int j = 0; j < data.NDim(); ++j)
             x[j] = *data.GetCoordComponent(i, j);
#ifdef USE_PARAMCACHE
          fval = func ( x.data() );
#else
          fval = func ( x.data(), p );
#endif

          // one -dim case
       } else {
          const auto x = data.GetCoordComponent(i, 0);
#ifdef USE_PARAMCACHE
          fval = func ( x );
#else
          fval = func ( x, p );
#endif

       }

      if (normalizeFunc) fval = fval * (1/norm);

      // function EvalLog protects against negative or too small values of fval
      double logval =  ROOT::Math::Util::EvalLog( fval);
      if (iWeight > 0) {
         double weight = data.Weight(i);
         logval *= weight;
         if (iWeight ==2) {
            logval *= weight; // use square of weights in likelihood
            if (!extended) {
               // needed sum of weights and sum of weight square if likelkihood is extended
               W = weight;
               W2 = weight*weight;
            }
         }
      }
   nPoints++;
      return LikelihoodAux<double>(logval, W, W2);
   };

#ifdef R__USE_IMT
  auto redFunction = [](const std::vector<LikelihoodAux<double>> & objs){
           return std::accumulate(objs.begin(), objs.end(), LikelihoodAux<double>(0.0,0.0,0.0),
                       [](const LikelihoodAux<double> &l1, const LikelihoodAux<double> &l2){
                           return l1+l2;
                  });
  };
#else
  (void)nChunks;
#endif

  double logl{};
  double sumW{};
  double sumW2{};
  if(executionPolicy == ROOT::Fit::ExecutionPolicy::kSerial){
    for (unsigned int i=0; i<n; ++i) {
      auto resArray = mapFunction(i);
      logl+=resArray.logvalue;
      sumW+=resArray.weight;
      sumW2+=resArray.weight2;
    }
#ifdef R__USE_IMT
  } else if(executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
    auto chunks = nChunks !=0? nChunks: setAutomaticChunking(data.Size());
    ROOT::TThreadExecutor pool;
    auto resArray = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, n), redFunction, chunks);
    logl=resArray.logvalue;
    sumW=resArray.weight;
    sumW2=resArray.weight2;
#endif
//   } else if(executionPolicy == ROOT::Fit::kMultitProcess){
    // ROOT::TProcessExecutor pool;
    // res = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, n), redFunction);
  } else{
    Error("FitUtil::EvaluateLogL","Execution policy unknown. Avalaible choices:\n ROOT::Fit::ExecutionPolicy::kSerial (default)\n ROOT::Fit::ExecutionPolicy::kMultithread (requires IMT)\n");
  }

  if (extended) {
      // add Poisson extended term
      double extendedTerm = 0; // extended term in likelihood
      double nuTot = 0;
      // nuTot is integral of function in the range
      // if function has been normalized integral has been already computed
      if (!normalizeFunc) {
         IntegralEvaluator<> igEval( func, p, true);
         std::vector<double> xmin(data.NDim());
         std::vector<double> xmax(data.NDim());

         // compute integral in the ranges where is defined
         if (data.Range().Size() > 0 ) {
            nuTot = 0;
            for (unsigned int ir = 0; ir < data.Range().Size(); ++ir) {
               data.Range().GetRange(&xmin[0],&xmax[0],ir);
               nuTot += igEval.Integral(xmin.data(),xmax.data());
            }
         } else {
            // use (-inf +inf)
            data.Range().GetRange(&xmin[0],&xmax[0]);
            // check if funcition is zero at +- inf
            if (func(xmin.data(), p) != 0 || func(xmax.data(), p) != 0) {
               MATH_ERROR_MSG("FitUtil::EvaluateLogLikelihood","A range has not been set and the function is not zero at +/- inf");
               return 0;
            }
            nuTot = igEval.Integral(&xmin[0],&xmax[0]);
         }

         // force to be last parameter value
         //nutot = p[func.NDim()-1];
         if (iWeight != 2)
            extendedTerm = - nuTot;  // no need to add in this case n log(nu) since is already computed before
         else {
            // case use weight square in likelihood : compute total effective weight = sw2/sw
            // ignore for the moment case when sumW is zero
            extendedTerm = - (sumW2 / sumW) * nuTot;
         }

      }
      else {
         nuTot = norm;
         extendedTerm = - nuTot + double(n) *  ROOT::Math::Util::EvalLog( nuTot);
         // in case of weights need to use here sum of weights (to be done)
      }
      logl += extendedTerm;

   }

   // reset the number of fitting data points
  //  nPoints = n;
// std::cout<<", n: "<<nPoints<<std::endl;
nPoints = 0;

   return -logl;
}

void FitUtil::EvaluateLogLGradient(const IModelFunction & f, const UnBinData & data, const double * p, double * grad, unsigned int & ) {
   // evaluate the gradient of the log likelihood function

   const IGradModelFunction * fg = dynamic_cast<const IGradModelFunction *>( &f);
   assert (fg != 0); // must be called by a grad function
   const IGradModelFunction & func = *fg;

   unsigned int n = data.Size();
   //int nRejected = 0;

   unsigned int npar = func.NPar();
   std::vector<double> gradFunc( npar );
   std::vector<double> g( npar);

   for (unsigned int i = 0; i < n; ++ i) {
      const double * x = data.Coords(i);
      double fval = func ( x , p);
      func.ParameterGradient( x, p, &gradFunc[0] );
      for (unsigned int kpar = 0; kpar < npar; ++ kpar) {
         if (fval > 0)
            g[kpar] -= 1./fval * gradFunc[ kpar ];
         else if (gradFunc [ kpar] != 0) {
            const double kdmax1 = std::sqrt( std::numeric_limits<double>::max() );
            const double kdmax2 = std::numeric_limits<double>::max() / (4*n);
            double gg = kdmax1 * gradFunc[ kpar ];
            if ( gg > 0) gg = std::min( gg, kdmax2);
            else gg = std::max(gg, - kdmax2);
            g[kpar] -= gg;
         }
         // if func derivative is zero term is also zero so do not add in g[kpar]
      }

    // copy result
   std::copy(g.begin(), g.end(), grad);
   }
}
//_________________________________________________________________________________________________
// for binned log likelihood functions
////////////////////////////////////////////////////////////////////////////////
/// evaluate the pdf (Poisson) contribution to the logl (return actually log of pdf)
/// and its gradient

double FitUtil::EvaluatePoissonBinPdf(const IModelFunction & func, const BinData & data, const double * p, unsigned int i, double * g ) {
   double y = 0;
   const double * x1 = data.GetPoint(i,y);

   const DataOptions & fitOpt = data.Opt();
   bool useBinIntegral = fitOpt.fIntegral && data.HasBinEdges();
   bool useBinVolume = (fitOpt.fBinVolume && data.HasBinEdges());

   IntegralEvaluator<> igEval( func, p, useBinIntegral);
   double fval = 0;
   const double * x2 = 0;
   // calculate the bin volume
   double binVolume = 1;
   std::vector<double> xc;
   if (useBinVolume) {
      unsigned int ndim = data.NDim();
      xc.resize(ndim);
      x2 = data.BinUpEdge(i);
      for (unsigned int j = 0; j < ndim; ++j) {
         binVolume *= std::abs( x2[j]-x1[j] );
         xc[j] = 0.5*(x2[j]+ x1[j]);
      }
      // normalize the bin volume using a reference value
      binVolume /= data.RefVolume();
   }

   const double * x = (useBinVolume) ? &xc.front() : x1;

   if (!useBinIntegral ) {
      fval = func ( x, p );
   }
   else {
      // calculate integral normalized (divided by bin volume)
      x2 =  data.BinUpEdge(i);
      fval = igEval( x1, x2 ) ;
   }
   if (useBinVolume) fval *= binVolume;

   // logPdf for Poisson: ignore constant term depending on N
   fval = std::max(fval, 0.0);  // avoid negative or too small values
   double logPdf =  - fval;
   if (y > 0.0) {
      // include also constants due to saturate model (see Baker-Cousins paper)
      logPdf += y * ROOT::Math::Util::EvalLog( fval / y) + y;
   }
   // need to return the pdf contribution (not the log)

   //double pdfval =  std::exp(logPdf);

  //if (g == 0) return pdfval;
   if (g == 0) return logPdf;

   unsigned int npar = func.NPar();
   const IGradModelFunction * gfunc = dynamic_cast<const IGradModelFunction *>( &func);

   // gradient  calculation
   if (gfunc != 0) {
      //case function provides gradient
      if (!useBinIntegral )
         gfunc->ParameterGradient(  x , p, g );
      else {
         // needs to calculate the integral for each partial derivative
         CalculateGradientIntegral( *gfunc, x1, x2, p, g);
      }

   }
   else {
      SimpleGradientCalculator  gc(func.NPar(), func);
      if (!useBinIntegral )
         gc.ParameterGradient(x, p, fval, g);
      else {
        // needs to calculate the integral for each partial derivative
         CalculateGradientIntegral( gc, x1, x2, p, g);
      }
   }
   // correct g[] do be derivative of poisson term
   for (unsigned int k = 0; k < npar; ++k) {
      // apply bin volume correction
      if (useBinVolume) g[k] *= binVolume;

      // correct for Poisson term
      if ( fval > 0)
         g[k] *= ( y/fval - 1.) ;//* pdfval;
      else if (y > 0) {
         const double kdmax1 = std::sqrt( std::numeric_limits<double>::max() );
         g[k] *= kdmax1;
      }
      else   // y == 0 cannot have  negative y
         g[k] *= -1;
   }


#ifdef DEBUG
   std::cout << "x = " << x[0] << " logPdf = " << logPdf << " grad";
   for (unsigned int ipar = 0; ipar < npar; ++ipar)
      std::cout << g[ipar] << "\t";
   std::cout << std::endl;
#endif

//   return pdfval;
   return logPdf;
}

double FitUtil::EvaluatePoissonLogL(const IModelFunction &func, const BinData &data, const double *p, int iWeight, bool extended,
                                    unsigned int &nPoints, const ROOT::Fit::ExecutionPolicy &executionPolicy, unsigned nChunks) {
   // evaluate the Poisson Log Likelihood
   // for binned likelihood fits
   // this is Sum ( f(x_i)  -  y_i * log( f (x_i) ) )
   // add as well constant term for saturated model to make it like a Chi2/2
   // by default is etended. If extended is false the fit is not extended and
   // the global poisson term is removed (i.e is a binomial fit)
   // (remember that in this case one needs to have a function with a fixed normalization
   // like in a non extended binned fit)
   //
   // if use Weight use a weighted dataset
   // iWeight = 1 ==> logL = Sum( w f(x_i) )
   // case of iWeight==1 is actually identical to weight==0
   // iWeight = 2 ==> logL = Sum( w*w * f(x_i) )
   //
   // nPoints returns the points where bin content is not zero


   unsigned int n = data.Size();

#ifdef USE_PARAMCACHE
   (const_cast<IModelFunction &>(func)).SetParameters(p);
#endif

   nPoints = 0;  // npoints


   // get fit option and check case of using integral of bins
   const DataOptions &fitOpt = data.Opt();
   bool useBinIntegral = fitOpt.fIntegral && data.HasBinEdges();
   bool useBinVolume = (fitOpt.fBinVolume && data.HasBinEdges());
   bool useW2 = (iWeight == 2);

   // normalize if needed by a reference volume value
   double wrefVolume = 1.0;
   if (useBinVolume) {
      if (fitOpt.fNormBinVolume) wrefVolume /= data.RefVolume();
   }

#ifdef DEBUG
   std::cout << "Evaluate PoissonLogL for params = [ ";
   for (unsigned int j = 0; j < func.NPar(); ++j) std::cout << p[j] << " , ";
   std::cout << "]  - data size = " << n << " useBinIntegral " << useBinIntegral << " useBinVolume "
             << useBinVolume << " useW2 " << useW2 << " wrefVolume = " << wrefVolume << std::endl;
#endif

#ifdef USE_PARAMCACHE
   IntegralEvaluator<> igEval(func, 0, useBinIntegral);
#else
   IntegralEvaluator<> igEval(func, p, useBinIntegral);
#endif
   // double nuTot = 0; // total number of expected events (needed for non-extended fits)
   // double wTot = 0; // sum of all weights
   // double w2Tot = 0; // sum of weight squared  (these are needed for useW2)

   auto mapFunction = [&](const unsigned i) {
      auto x1 = data.GetCoordComponent(i, 0);
      auto y = *data.ValuePtr(i);

      const double *x = nullptr;
      std::vector<double> xc;
      double fval = 0;
      double binVolume = 1.0;

      if (useBinVolume) {
         unsigned int ndim = data.NDim();
         const double *x2 = data.BinUpEdge(i);
         xc.resize(data.NDim());
         for (unsigned int j = 0; j < ndim; ++j) {
            auto xx = *data.GetCoordComponent(i, j);
            binVolume *= std::abs(x2[j] - xx);
            xc[j] = 0.5 * (x2[j] + xx);
         }
         x = xc.data();
         // normalize the bin volume using a reference value
         binVolume *= wrefVolume;
      } else if (data.NDim() > 1) {
         xc.resize(data.NDim());
         xc[0] = *x1;
         for (unsigned int j = 1; j < data.NDim(); ++j) {
            xc[j] = *data.GetCoordComponent(i, j);
         }
         x = xc.data();
      } else {
         x = x1;
      }

      if (!useBinIntegral) {
#ifdef USE_PARAMCACHE
         fval = func(x);
#else
         fval = func(x, p);
#endif
      } else {
         // calculate integral (normalized by bin volume)
         // need to set function and parameters here in case loop is parallelized
         fval = igEval(x, data.BinUpEdge(i));
      }
      if (useBinVolume) fval *= binVolume;



#ifdef DEBUG
      int NSAMPLE = 100;
      if (i % NSAMPLE == 0) {
         std::cout << "evt " << i << " x = [ ";
         for (unsigned int j = 0; j < func.NDim(); ++j) std::cout << x[j] << " , ";
         std::cout << "]  ";
         if (fitOpt.fIntegral) {
            std::cout << "x2 = [ ";
            for (unsigned int j = 0; j < func.NDim(); ++j) std::cout << data.BinUpEdge(i)[j] << " , ";
            std::cout << "] ";
         }
         std::cout << "  y = " << y << " fval = " << fval << std::endl;
      }
#endif


      // EvalLog protects against 0 values of fval but don't want to add in the -log sum
      // negative values of fval
      fval = std::max(fval, 0.0);

      double nloglike = 0; // negative loglikelihood
      if (useW2) {
         // apply weight correction . Effective weight is error^2/ y
         // and expected events in bins is fval/weight
         // can apply correction only when y is not zero otherwise weight is undefined
         // (in case of weighted likelihood I don't care about the constant term due to
         // the saturated model)
         if (y != 0) {
            auto pError = data.ErrorPtr(i);
            auto error = (pError != nullptr) ? *pError : 1.;
            double weight = (error * error) / y; // this is the bin effective weight
            if (extended) {
               nloglike = fval * weight;
               // wTot  += weight;
               // w2Tot += weight*weight;
            }
            nloglike -= weight * y * ROOT::Math::Util::EvalLog(fval);
         }

         //  need to compute total weight and weight-square
         // if (extended ) {
         //    nuTot += fval;
         // }

      } else {
         // standard case no weights or iWeight=1
         // this is needed for Poisson likelihood (which are extened and not for multinomial)
         // the formula below  include constant term due to likelihood of saturated model (f(x) = y)
         // (same formula as in Baker-Cousins paper, page 439 except a factor of 2
         if (extended) nloglike = fval - y;

         if (y >  0) {
            nloglike += y * (ROOT::Math::Util::EvalLog(y) - ROOT::Math::Util::EvalLog(fval));
            nPoints++;
         }
      }
      return nloglike;
   };

   // if (notExtended) {
   //    // not extended : remove from the Likelihood the global Poisson term
   //    if (!useW2)
   //       nloglike -= nuTot - yTot * ROOT::Math::Util::EvalLog( nuTot);
   //    else {
   //       // this needs to be checked
   //       nloglike -= wTot* nuTot - wTot* yTot * ROOT::Math::Util::EvalLog( nuTot);
   //    }

   // }

   // if (extended && useW2) {
   //    // effective total weight is total sum of weight square / sum of weights
   //    //nloglike += (w2Tot/wTot) * nuTot;
   // }
#ifdef R__USE_IMT
   auto redFunction = [](const std::vector<double> &objs) {
      return std::accumulate(objs.begin(), objs.end(), double{});
   };
#else
   (void)nChunks;
#endif

   double res{};
   if (executionPolicy == ROOT::Fit::ExecutionPolicy::kSerial) {
      for (unsigned int i = 0; i < n; ++i) {
         res += mapFunction(i);
      }
#ifdef R__USE_IMT
   } else if (executionPolicy == ROOT::Fit::ExecutionPolicy::kMultithread) {
      auto chunks = nChunks != 0 ? nChunks : setAutomaticChunking(data.Size());
      ROOT::TThreadExecutor pool;
      res = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, n), redFunction, chunks);
#endif
      //   } else if(executionPolicy == ROOT::Fit::kMultitProcess){
      // ROOT::TProcessExecutor pool;
      // res = pool.MapReduce(mapFunction, ROOT::TSeq<unsigned>(0, n), redFunction);
   } else {
      Error("FitUtil::EvaluatePoissonLogL",
            "Execution policy unknown. Avalaible choices:\n ROOT::Fit::ExecutionPolicy::kSerial (default)\n ROOT::Fit::ExecutionPolicy::kMultithread (requires IMT)\n");
   }

#ifdef DEBUG
   std::cout << "Loglikelihood  = " << res << std::endl;
#endif

   return res;
}

void FitUtil::EvaluatePoissonLogLGradient(const IModelFunction & f, const BinData & data, const double * p, double * grad ) {
   // evaluate the gradient of the Poisson log likelihood function

   const IGradModelFunction * fg = dynamic_cast<const IGradModelFunction *>( &f);
   assert (fg != 0); // must be called by a grad function
   const IGradModelFunction & func = *fg;

   unsigned int n = data.Size();

   const DataOptions & fitOpt = data.Opt();
   bool useBinIntegral = fitOpt.fIntegral && data.HasBinEdges();
   bool useBinVolume = (fitOpt.fBinVolume && data.HasBinEdges());

   double wrefVolume = 1.0;
   std::vector<double> xc;
   if (useBinVolume) {
      if (fitOpt.fNormBinVolume) wrefVolume /= data.RefVolume();
      xc.resize(data.NDim() );
   }

   IntegralEvaluator<> igEval( func, p, useBinIntegral);

   unsigned int npar = func.NPar();
   std::vector<double> gradFunc( npar );
   std::vector<double> g( npar);

   for (unsigned int i = 0; i < n; ++ i) {
      const double * x1 = data.Coords(i);
      double y = data.Value(i);
      double fval = 0;
      const double * x2 = 0;

      double binVolume = 1.0;
      if (useBinVolume) {
         x2 = data.BinUpEdge(i);
         unsigned int ndim = data.NDim();
         for (unsigned int j = 0; j < ndim; ++j) {
            binVolume *= std::abs( x2[j]-x1[j] );
            xc[j] = 0.5*(x2[j]+ x1[j]);
         }
         // normalize the bin volume using a reference value
         binVolume *= wrefVolume;
      }

      const double * x = (useBinVolume) ? &xc.front() : x1;

      if (!useBinIntegral) {
         fval = func ( x, p );
         func.ParameterGradient(  x , p, &gradFunc[0] );
      }
      else {
         // calculate integral (normalized by bin volume)
         // need to set function and parameters here in case loop is parallelized
         x2 = data.BinUpEdge(i);
         fval = igEval( x1, x2) ;
         CalculateGradientIntegral( func, x1, x2, p, &gradFunc[0]);
      }
      if (useBinVolume) fval *= binVolume;

      // correct the gradient
      for (unsigned int kpar = 0; kpar < npar; ++ kpar) {

         // correct gradient for bin volumes
         if (useBinVolume) gradFunc[kpar] *= binVolume;

         // df/dp * (1.  - y/f )
         if (fval > 0)
            g[kpar] += gradFunc[ kpar ] * ( 1. - y/fval );
         else if (gradFunc [ kpar] != 0) {
            const double kdmax1 = std::sqrt( std::numeric_limits<double>::max() );
            const double kdmax2 = std::numeric_limits<double>::max() / (4*n);
            double gg = kdmax1 * gradFunc[ kpar ];
            if ( gg > 0) gg = std::min( gg, kdmax2);
            else gg = std::max(gg, - kdmax2);
            g[kpar] -= gg;
         }
      }

      // copy result
      std::copy(g.begin(), g.end(), grad);
   }
}

unsigned FitUtil::setAutomaticChunking(unsigned nEvents){
      SysInfo_t s;
      gSystem->GetSysInfo(&s);
      auto ncpu  = s.fCpus;
      return ((nEvents/ncpu + 1) % 1000) *40 ; //arbitrary formula
}

}

} // end namespace ROOT

