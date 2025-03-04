/** \file   GenericReconCartesianGrappaAIGadget.h
    \brief  This is the class gadget for both 2DT and 3DT cartesian grappa ai reconstruction, working on the mrd::ReconData.
    \author Hui Xue
*/

#pragma once

#include "GenericReconCartesianGrappaGadget.h"
#include "python_toolbox.h"

namespace Gadgetron {

    class GenericReconCartesianGrappaAIGadget : public GenericReconCartesianGrappaGadget
    {
    public:
        typedef GenericReconCartesianGrappaGadget BaseClass;
        typedef typename BaseClass::ReconObjType ReconObjType;
        typedef std::complex<float> T;

        GenericReconCartesianGrappaAIGadget(const Core::Context& context, const Core::GadgetProperties& properties);

    protected:

        // --------------------------------------------------
        // gadget functions
        // --------------------------------------------------
        /// [Nor1 Sor1 SLC]
        std::vector< std::vector< hoNDArray<T> > > kernels_;
        std::vector< std::vector<boost::python::object> > models_;
        /// [RO E1 E2 1 N S SLC]
        std::vector<mrd::ImageArray> recon_res_grappa_ai_;

        // gadgetron home
        std::string gt_home_;

        // default interface function
        void process(Core::InputChannel<mrd::ReconData>& in, Core::OutputChannel& out) override;

        // calibration, if only one dst channel is prescribed, the GrappaOne is used
        virtual void perform_calib(mrd::ReconAssembly& recon_bit, ReconObjType& recon_obj, size_t encoding);

        // unwrapping or coil combination
        virtual void perform_unwrapping(mrd::ReconAssembly& recon_bit, ReconObjType& recon_obj, size_t encoding);
    };
}
