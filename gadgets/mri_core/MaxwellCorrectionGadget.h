/**
    \brief  Performs Maxwell correction based on MaxwellCorrection coefficients stored in the MRD Header's UserParameters.
    \test   Tested by: gpu_spiral_sb.cfg and gpu_spiral.cfg
*/

#pragma once

#include "Node.h"
#include "hoNDArray.h"
#include <numeric>
#ifdef USE_OMP
#include <omp.h>
#endif

namespace Gadgetron {
class MaxwellCorrectionGadget : public Core::ChannelGadget<mrd::Image<std::complex<float>>> {
  public:
    using Core::ChannelGadget<mrd::Image<std::complex<float>>>::ChannelGadget;
    MaxwellCorrectionGadget(const Core::Context& context, const Core::GadgetProperties& props);
    ~MaxwellCorrectionGadget() override = default;
    void process(Core::InputChannel<mrd::Image<std::complex<float>>>& input, Core::OutputChannel& output) override;
    void patient_to_physical_coordinate(std::vector<float> &norm_vec, mrd::PatientPosition patient_position);
    void find_flow_dir(mrd::Image<std::complex<float>> m1);
  protected:
    std::vector<double> maxwell_coefficients_;
	std::vector<float> RO_dir_Physical_;
	std::vector<float> PE_dir_Physical_;
	std::vector<float> SLC_dir_Physical_;
	std::vector<float> SLC_position_Physical_;
    bool FlipPhaseDirection_ = false;
    int FlowDirection_ = 4; //flow encoding direction: 4 through plane, 2 RO direction, 1 PE direction
	mrd::PatientPosition patient_position_;
	bool maxwell_coefficients_present_;
};
} // namespace Gadgetron
