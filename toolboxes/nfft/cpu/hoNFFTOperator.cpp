#include "cpunfft_export.h"
#include "hoNDArray_math.h"
#include "hoNFFT.h"
#include "../NFFTOperator.hpp"

namespace Gadgetron{
    template EXPORTCPUNFFT class NFFTOperator<hoNDArray,float,1>;
    template EXPORTCPUNFFT class NFFTOperator<hoNDArray,float,2>;
    template EXPORTCPUNFFT class NFFTOperator<hoNDArray,float,3>;

    template EXPORTCPUNFFT class NFFTOperator<hoNDArray,double,1>;
    template EXPORTCPUNFFT class NFFTOperator<hoNDArray,double,2>;
    template EXPORTCPUNFFT class NFFTOperator<hoNDArray,double,3>;
}

