//    TO DO: - For 3D sequences (E2>1), the 3 navigators should be equivalent for all e2 partition
//             encoding steps (other than the mean phase).  So we should average across them.  I guess
//             one way to do so would be to include all partition encoding steps (that have been acquired
//             up to that one), also from the previous repetitions, in the robust fit, being careful with
//             the column representing the slope.  The problem with that is that the matrix to invert is
//             longer, so it could take longer to compute.
//           - Test the case that more repetitions are sent than the number specified in the xml header.

#include "EPICorrGadget.h"
#include "hoNDArray_fileio.h"

namespace Gadgetron {

#define OE_PHASE_CORR_POLY_ORDER 4

    EPICorrGadget::EPICorrGadget(const Core::Context& context, const Core::GadgetProperties& props)
        : Core::ChannelGadget<mrd::Acquisition>(context, props)
        , corrComputed_(false), navNumber_(-1), epiEchoNumber_(-1)
    {
        auto& h = context.header;

        if (h.encoding.size() == 0) {
            GDEBUG("Number of encoding spaces: %d\n", h.encoding.size());
            GADGET_THROW("This Gadget needs an encoding description\n");
        }

        // Get the encoding space and trajectory description
        mrd::EncodingSpaceType e_space = h.encoding[0].encoded_space;
        mrd::EncodingSpaceType r_space = h.encoding[0].recon_space;
        mrd::EncodingLimitsType e_limits = h.encoding[0].encoding_limits;
        mrd::TrajectoryDescriptionType traj_desc;

        if (h.encoding[0].trajectory_description) {
            traj_desc = *h.encoding[0].trajectory_description;
        } else {
            GADGET_THROW("Trajectory description missing");
        }

        if (traj_desc.identifier != "ConventionalEPI") {
            GADGET_THROW("Expected trajectory description identifier 'ConventionalEPI', not found.");
        }


        for (std::vector<mrd::UserParameterLongType>::iterator i(traj_desc.user_parameter_long.begin());
             i != traj_desc.user_parameter_long.end(); ++i) {
            if (i->name == "numberOfNavigators") {
                numNavigators_ = i->value;
            } else if (i->name == "etl") {
                etl_ = i->value;
            }
        }

        // Make sure the reference navigator is properly set:
        if (referenceNavigatorNumber > (numNavigators_ - 1)) {
            GADGET_THROW("Reference navigator number is larger than number of navigators acquired.");
        }

        // Initialize arrays needed for temporal filtering, if requested:
        GDEBUG_STREAM("navigatorParameterFilterLength = " << navigatorParameterFilterLength);
        if (navigatorParameterFilterLength > 1) {
            init_arrays_for_nav_parameter_filtering(e_limits);
        }

        GDEBUG_STREAM("EPICorrGadget configured");
    }

    void EPICorrGadget::process(Core::InputChannel<mrd::Acquisition>& input, Core::OutputChannel& out)
    {
        std::vector<mrd::Acquisition> unprocessed_acquisitions;

        for (auto acq: input) {
            // Get a reference to the acquisition header
            auto& hdr = acq.head;
            auto& data = acq.data;

            // Pass on the non-EPI data (e.g. FLASH Calibration)
            if (hdr.encoding_space_ref > 0) {
                out.push(std::move(acq));
                continue;
            }

            // Check to see if the data is a navigator line or an imaging line
            if (hdr.flags.HasFlags(mrd::AcquisitionFlags::kIsPhasecorrData)) {
                arma::cx_fmat adata = as_arma_matrix(data);
                process_phase_correction_data(hdr, adata);
            } else {

                unprocessed_acquisitions.push_back(std::move(acq));
                if (corrComputed_) {
                    for (auto& msg : unprocessed_acquisitions) {

                        arma::cx_fmat adata = as_arma_matrix(msg.data);

                        mrd::AcquisitionHeader &hdr = msg.head;
                        apply_epi_correction(hdr, adata);

                        out.push(std::move(msg));
                    }
                    unprocessed_acquisitions.clear();
                }
            }
        }
    }

    void EPICorrGadget::apply_epi_correction(mrd::AcquisitionHeader &hdr, arma::cx_fmat &adata)
    {
        // Increment the echo number
        epiEchoNumber_ += 1;

        if (epiEchoNumber_ == 0) {
            // For now, we will correct the phase evolution of each EPI line, with respect
            //   to the first line in the EPI readout train (echo 0), due to B0 inhomogeneities.
            //   That is, the reconstructed images will have the phase that the object had at
            //   the beginning of the EPI readout train (excluding the phase due to encoding),
            //   multiplied by the coil phase.
            // Later, we could add the time between the excitation and echo 0, or between one
            //   of the navigators and echo 0, to correct for phase differences from shot to shot.
            //   This will be important for multi-shot EPI acquisitions.
            RefNav_to_Echo0_time_ES_ = 0;
        }

        // Apply the correction
        // We use the armadillo notation that loops over all the columns
        if (hdr.flags.HasFlags(mrd::AcquisitionFlags::kIsReverse)) {
            // Negative readout
            for (int p = 0; p < adata.n_cols; p++) {
                adata.col(p) %= (pow(corrB0_, epiEchoNumber_ + RefNav_to_Echo0_time_ES_) % corrneg_);
            }
            // Now that we have corrected we set the readout direction to positive
            hdr.flags.UnsetFlags(mrd::AcquisitionFlags::kIsReverse);
        } else {
            // Positive readout
            for (int p = 0; p < adata.n_cols; p++) {
                adata.col(p) %= (pow(corrB0_, epiEchoNumber_ + RefNav_to_Echo0_time_ES_) % corrpos_);
            }
        }
    }

    void EPICorrGadget::process_phase_correction_data(mrd::AcquisitionHeader &hdr, arma::cx_fmat &adata)
    {
        // Increment the navigator counter
        navNumber_ += 1;

        // If the number of navigators per shot is exceeded, then
        // we are at the beginning of the next shot
        if (navNumber_ == numNavigators_) {
            corrComputed_ = false;
            navNumber_ = 0;
            epiEchoNumber_ = -1;
        }

        int Nx_ = adata.n_rows;

        // If we are at the beginning of a shot, then initialize
        if (navNumber_ == 0) {
            // Set the size of the corrections and storage arrays
            corrB0_.set_size(Nx_);
            corrpos_.set_size(Nx_);
            corrneg_.set_size(Nx_);
            navdata_.set_size(Nx_, hdr.channel_order.size(), numNavigators_);
            navdata_.zeros();
            // Store the first navigator's polarity
            startNegative_ = hdr.flags.HasFlags(mrd::AcquisitionFlags::kIsReverse);
        }

        // Store the navigator data
        navdata_.slice(navNumber_) = adata;

        // If this is the last of the navigators for this shot, then
        // compute the correction operator
        if (navNumber_ == (numNavigators_ - 1)) {

            arma::fvec tvec = arma::zeros<arma::fvec>(Nx_);            // temp column real
            arma::fvec x = arma::linspace<arma::fvec>(-0.5, 0.5, Nx_); // Evenly spaced x-space locations

            int p; // counter

            // mean of the reference navigator (across RO and channels):
            std::complex<float> navMean = mean(vectorise(navdata_.slice(referenceNavigatorNumber)));
    
            // for clarity, we'll use the following when filtering navigator parameters:
            size_t set(hdr.idx.set.value_or(0)), slc(hdr.idx.slice.value_or(0)), exc(0);
            if (navigatorParameterFilterLength > 1) {
                // Careful: kspace_encode_step_2 for a navigator is always 0, and at this point we
                //          don't have access to the kspace_encode_step_2 for the next line.  Instead,
                //          keep track of the excitation number for this set and slice:
                //size_t e2  = hdr.idx.kspace_encode_step_2;
                //size_t rep = hdr.idx.repetition;
                exc = excitNo_[slc][set];   // excitation number with this same specific set and slc
                //GDEBUG_STREAM("Excitation number:" << exc << "; slice: " << slc);

                // If, for whatever reason, we are getting more repetitions than the header
                //   specified, increase the size of the array to accommodate:
                if (exc >= (Nav_mag_.get_size(0) / E2_)) {
                    increase_no_repetitions(100);     // add 100 volumes more, to be safe
                }
                Nav_mag_(exc, set, slc) = abs(navMean);
            }


            /////////////////////////////////////
            //////      B0 correction      //////
            /////////////////////////////////////

            if (B0CorrectionMode.compare("none") != 0)    // If B0 correction is requested
            {

                arma::cx_fvec ctemp = arma::zeros<arma::cx_fvec>(Nx_);    // temp column complex
                // Accumulate over navigator pairs and sum over coils
                // this is the average phase difference between consecutive odd or even navigators
                for (p = 0; p < numNavigators_ - 2; p++) {
                    ctemp += sum(conj(navdata_.slice(p)) % navdata_.slice(p + 2), 1);
                }

                // Perform the fit:
                float slope = 0.;
                float intercept = 0.;
                if ((B0CorrectionMode.compare("mean") == 0) ||
                    (B0CorrectionMode.compare("linear") == 0)) {
                    // If a linear term is requested, compute it first (in the complex domain):
                    if (B0CorrectionMode.compare("linear") == 0) {          // Robust fit to a straight line:
                        slope = (Nx_ - 1) * std::arg(arma::cdot(ctemp.rows(0, Nx_ - 2), ctemp.rows(1, Nx_ - 1)));
                        //GDEBUG_STREAM("Slope = " << slope << std::endl);
                        // If we need to filter the estimate:
                        if (navigatorParameterFilterLength > 1) {
                            // (Because to estimate the intercept (constant term) we need to use the slope estimate,
                            //   we want to filter it first):
                            //   - Store the value in the corresponding array (we want to store it before filtering)
                            B0_slope_(exc, set, slc) = slope;
                            //   - Filter parameter:
                            slope = filter_nav_correction_parameter(B0_slope_, Nav_mag_, exc, set, slc,
                                                                    navigatorParameterFilterLength);
                        }

                        // Correct for the slope, to be able to compute the average phase:
                        ctemp = ctemp % exp(arma::cx_fvec(arma::zeros<arma::fvec>(Nx_), -slope * x));
                    }   // end of the B0CorrectionMode == "linear"

                    // Now, compute the mean phase:
                    intercept = arg(sum(ctemp));
                    //GDEBUG_STREAM("Intercept = " << intercept << std::endl);
                    if (navigatorParameterFilterLength > 1) {
                        //   - Store the value found in the corresponding array:
                        B0_intercept_(exc, set, slc) = intercept;
                        //   - Filter parameters:
                        // Filter in the complex domain (last arg:"true"), to avoid smoothing across phase wraps:
                        intercept = filter_nav_correction_parameter(B0_intercept_, Nav_mag_, exc, set, slc,
                                                                    navigatorParameterFilterLength, true);
                    }

                    // Then, our estimate of the phase:
                    tvec = slope * x + intercept;

                }       // end of B0CorrectionMode == "mean" or "linear"

                // The B0 Correction:
                // 0.5* because what we have calculated was the phase difference between every other navigator
                corrB0_ = exp(arma::cx_fvec(arma::zeros<arma::fvec>(ctemp.n_rows), -0.5 * tvec));

            }        // end of B0CorrectionMode != "none"
            else {      // No B0 correction:
                corrB0_.ones();
            }


            ////////////////////////////////////////////////////
            //////      Odd-Even correction -- Phase      //////
            ////////////////////////////////////////////////////

            if (OEPhaseCorrectionMode.compare("none") != 0)    // If Odd-Even phase correction is requested
            {
                // Accumulate over navigator triplets and sum over coils
                // this is the average phase difference between odd and even navigators
                // Note: we have to correct for the B0 evolution between navigators before
                arma::cx_fvec ctemp = arma::zeros<arma::cx_fvec>(Nx_);     // set all elements to zero
                for (p = 0; p < numNavigators_ - 2; p = p + 2) {
                    ctemp += sum(conj(navdata_.slice(p) / repmat(corrB0_, 1, navdata_.n_cols) +
                                      navdata_.slice(p + 2) % repmat(
                                              corrB0_, 1, navdata_.n_cols)) % navdata_.slice(p + 1), 1);
                }

                float slope = 0.;
                float intercept = 0.;
                if ((OEPhaseCorrectionMode.compare("mean") == 0) ||
                    (OEPhaseCorrectionMode.compare("linear") == 0) ||
                    (OEPhaseCorrectionMode.compare("polynomial") == 0)) {
                    // If a linear term is requested, compute it first (in the complex domain):
                    // (This is important in case there are -pi/+pi phase wraps, since a polynomial
                    //  fit to the phase will not work)
                    if ((OEPhaseCorrectionMode.compare("linear") == 0) ||
                        (OEPhaseCorrectionMode.compare("polynomial") ==
                         0)) {          // Robust fit to a straight line:
                        slope = (Nx_ - 1) * std::arg(arma::cdot(ctemp.rows(0, Nx_ - 2), ctemp.rows(1, Nx_ - 1)));
                        // If we need to filter the estimate:
                        if (navigatorParameterFilterLength > 1) {
                            // (Because to estimate the intercept (constant term) we need to use the slope estimate,
                            //   we want to filter it first):
                            //   - Store the value in the corresponding array (we want to store it before filtering)
                            OE_phi_slope_(exc, set, slc) = slope;
                            //   - Filter parameter:
                            slope = filter_nav_correction_parameter(OE_phi_slope_, Nav_mag_, exc, set, slc,
                                                                    navigatorParameterFilterLength);
                        }

                        // Now correct for the slope, to be able to compute the average phase:
                        ctemp = ctemp % exp(arma::cx_fvec(arma::zeros<arma::fvec>(Nx_), -slope * x));
                        // at this point we should have got rid of any -pi/+pi phase wraps.
                    }   // end of the OEPhaseCorrectionMode == "linear" or "polynomial"

                    // Now, compute the mean phase:
                    intercept = arg(sum(ctemp));
                    //GDEBUG_STREAM("Intercept = " << intercept << std::endl);
                    if (navigatorParameterFilterLength > 1) {
                        //   - Store the value found in the corresponding array:
                        OE_phi_intercept_(exc, set, slc) = intercept;
                        //   - Filter parameters:
                        // Filter in the complex domain ("true"), to avoid smoothing across phase wraps:
                        intercept = filter_nav_correction_parameter(OE_phi_intercept_, Nav_mag_, exc, set, slc,
                                                                    navigatorParameterFilterLength, true);
                    }

                    // Then, our estimate of the phase:
                    tvec = slope * x + intercept;

                    // If a polynomial fit is requested:
                    if (OEPhaseCorrectionMode.compare("polynomial") == 0) {
                        tvec += polynomial_correction(Nx_, x, ctemp, set, slc, exc, intercept);

                    }   // end of OEPhaseCorrectionMode == "polynomial"

                }       // end of OEPhaseCorrectionMode == "mean", "linear" or "polynomial"

                if (!startNegative_) {
                    // if the first navigator is a positive readout, we need to flip the sign of our correction
                    tvec = -1.0 * tvec;
                }
            }    // end of OEPhaseCorrectionMode != "none"
            else {      // No OEPhase correction:
                tvec.zeros();
            }

            // Odd and even phase corrections
            corrpos_ = exp(arma::cx_fvec(arma::zeros<arma::fvec>(Nx_), -0.5 * tvec));
            corrneg_ = exp(arma::cx_fvec(arma::zeros<arma::fvec>(Nx_), +0.5 * tvec));
            corrComputed_ = true;

            // Increase the excitation number for this slice and set (to be used for the next shot)
            if (navigatorParameterFilterLength > 1) {
                excitNo_[slc][set]++;
            }
        }
    }

    arma::fvec
    EPICorrGadget::polynomial_correction(int Nx_, const arma::fvec &x, const arma::cx_fvec &ctemp_in, size_t set, size_t slc, size_t exc,
                                             float intercept)
    {
        // Fit the residuals (i.e., after removing the linear trend) to a polynomial.
        // You cannot fit the phase directly to the polynomial because it doesn't work
        //   in cases that the phase wraps across the image.
        // Since we have already removed the slope (in the if OEPhaseCorrectionMode
        //   == "linear" or "polynomial" step), just remove the constant phase:
        arma::cx_fvec ctemp = ctemp_in % exp(
                                arma::cx_fvec(arma::zeros<arma::fvec>(Nx_), -intercept * arma::ones<arma::fvec>(Nx_)));

        // Use the magnitude of the average odd navigator as weights:
        arma::fvec ctemp_odd = arma::zeros<arma::fvec>(
                                Nx_);    // temp column complex for odd  magnitudes
        for (int p = 0; p < numNavigators_ - 2; p = p + 2) {
                            ctemp_odd += (sqrt(sum(square(abs(navdata_.slice(p))), 1)) + sqrt(
                                    sum(square(abs(navdata_.slice(p + 2))), 1))) / 2;
                        }
        arma::fmat X;

        if (OEPhaseCorrectionMode.compare("polynomial") == 0) {

                X = arma::zeros<arma::fmat>(Nx_, OE_PHASE_CORR_POLY_ORDER + 1);
                X.col(0) = arma::ones<arma::fvec>(Nx_);
                X.col(1) = x;                       // x
                X.col(2) = square(x);         // x^2
                X.col(3) = x % X.col(2);            // x^3
                X.col(4) = square(X.col(2));  // x^4
            }
        arma::fmat WX = diagmat(ctemp_odd) * X;   // Weighted polynomial matrix
        arma::fvec Wctemp(Nx_);                           // Weighted phase residual
        for (int p = 0; p < Nx_; p++) {
                            Wctemp(p) = ctemp_odd(p) * arg(ctemp(p));
                        }

        // Solve for the polynomial coefficients:
        arma::fvec phase_poly_coef = solve(WX, Wctemp);
        if (navigatorParameterFilterLength > 1) {
                            for (size_t i = 0; i < OE_phi_poly_coef_.size(); ++i) {
                                //   - Store the value found in the corresponding array:
                                OE_phi_poly_coef_[i](exc, set, slc) = phase_poly_coef(i);

                                //   - Filter parameters:
                                phase_poly_coef(i) = filter_nav_correction_parameter(OE_phi_poly_coef_[i], Nav_mag_,
                                                                                     exc, set, slc,
                                                                                     navigatorParameterFilterLength);
                            }
                            //GDEBUG_STREAM("OE_phi_poly_coef size: " << OE_phi_poly_coef_.size());
                        }

        // Then, update our estimate of the phase correction:

        return X * phase_poly_coef;
    }


//////////////////////////////////////////////////////////
//
// init_arrays_for_nav_parameter_filtering
//
//    function to initialize the arrays that will be used for the navigator parameters filtering
//    - e_limits: encoding limits

    void EPICorrGadget::init_arrays_for_nav_parameter_filtering(mrd::EncodingLimitsType e_limits) {
        // TO DO: Take into account the acceleration along E2:

        E2_ = e_limits.kspace_encoding_step_2 ? e_limits.kspace_encoding_step_2->maximum -
                                                e_limits.kspace_encoding_step_2->minimum + 1 : 1;
        size_t REP = e_limits.repetition ? e_limits.repetition->maximum - e_limits.repetition->minimum + 1 : 1;
        size_t SET = e_limits.set ? e_limits.set->maximum - e_limits.set->minimum + 1 : 1;
        size_t SLC = e_limits.slice ? e_limits.slice->maximum - e_limits.slice->minimum + 1 : 1;
        // NOTE: For EPI sequences, "segment" indicates odd/even readout, so we don't need a separate dimension for it.
        GDEBUG_STREAM("E2: " << E2_ << "; SLC: " << SLC << "; REP: " << REP << "; SET: " << SET);

        // For 3D sequences, the e2 index in the navigator is always 0 (there is no phase encoding in
        //   the navigator), so we keep track of the excitation number for each slice and set) to do
        //   the filtering>
        excitNo_.resize(SLC);
        for (size_t i = 0; i < SLC; ++i) {
            excitNo_[i].resize(SET, size_t(0));
        }

        // For 3D sequences, all e2 phase encoding steps excite the whole volume, so the
        //   navigators should be the same.  So when we filter across repetitions, we have
        //   to do it also through e2.  Bottom line: e2 and repetition are equivalent.
        Nav_mag_.create(E2_ * REP, SET, SLC);
        B0_intercept_.create(E2_ * REP, SET, SLC);
        if (B0CorrectionMode.compare("linear") == 0) {
            B0_slope_.create(E2_ * REP, SET, SLC);
        }
        OE_phi_intercept_.create(E2_ * REP, SET, SLC);
        if ((OEPhaseCorrectionMode.compare("linear") == 0) ||
            (OEPhaseCorrectionMode.compare("polynomial") == 0)) {
            OE_phi_slope_.create(E2_ * REP, SET, SLC);
            if (OEPhaseCorrectionMode.compare("polynomial") == 0) {
                OE_phi_poly_coef_.resize(OE_PHASE_CORR_POLY_ORDER + 1);
                for (size_t i = 0; i < OE_phi_poly_coef_.size(); ++i) {
                    OE_phi_poly_coef_[i].create(E2_ * REP, SET, SLC);
                }
            }
        }

        // Armadillo vector of evenly-spaced timepoints to filter navigator parameters:
        t_ = arma::linspace<arma::fvec>(0, navigatorParameterFilterLength - 1,
                                        navigatorParameterFilterLength);

    }


////////////////////////////////////////////////////
//
//  filter_nav_correction_parameter
//
//    function to filter (over e2/repetition number) a navigator parameter.
//    - nav_corr_param_array: array of navigator parameters
//    - weights_array       : array with weights for the filtering
//    - exc                 : current excitation number (for this set and slice)
//    - set                 : set of the array to filter (current one)
//    - slc                 : slice of the array to filter (current one)
//    - Nt                  : number of e2/timepoints/repetitions to filter
//    - filter_in_complex_domain : whether to filter in the complex domain, to avoid +/- pi wraps (default: false)
//
//    Currently, it does a simple weighted linear fit.

    float EPICorrGadget::filter_nav_correction_parameter(hoNDArray<float> &nav_corr_param_array,
                                                         hoNDArray<float> &weights_array,
                                                         size_t exc,
                                                         size_t set,
                                                         size_t slc,
                                                         size_t Nt,
                                                         bool filter_in_complex_domain) {
        // If the array to be filtered doesn't have 3 dimensions, we are in big trouble:
        if (nav_corr_param_array.get_number_of_dimensions() != 3) {
            GERROR("cbi_EPICorrGadget::filter_nav_correction_parameter, incorrect number of dimensions of the array.\n");
            return -1;
        }

        // The dimensions of the weights array should be the same as the parameter array:
        if (!nav_corr_param_array.dimensions_equal(weights_array)) {
            GERROR("cbi_EPICorrGadget::filter_nav_correction_parameter, dimensions of the parameter and weights arrays don't match.\n");
            return -1;
        }

        // If this repetition number is less than then number of repetitions to exclude...
        if (exc < navigatorParameterFilterExcludeVols * E2_) {
            //   no filtering is needed, just return the corresponding value:
            return nav_corr_param_array(exc, set, slc);
        }

        // for now, just to a simple (robust) linear fit to the previous Nt timepoints:
        // TO DO: do we want to do something fancier?

        //
        // extract the timeseries (e2 phase encoding steps and repetitions)
        // of parameters and weights corresponding to the requested indices:

        // make sure we don't use more timepoints (e2 phase encoding steps and repetitions)
        //    that the currently acquired (minus the ones we have been asked to exclude
        //    from the beginning of the run):
        Nt = std::min(Nt, exc - (navigatorParameterFilterExcludeVols * E2_) + 1);

        // create armadillo vectors, and stuff them in reverse order (from the
        // current timepoint, looking backwards). This way, the filtered value
        // we want would be simply the intercept):
        arma::fvec weights = arma::zeros<arma::fvec>(Nt);
        arma::fvec params = arma::zeros<arma::fvec>(Nt);
        for (size_t t = 0; t < Nt; ++t) {
            weights(t) = weights_array(exc - t, set, slc);
            params(t) = nav_corr_param_array(exc - t, set, slc);
        }

        /////     weighted fit:          b = (W*[1 t_])\(W*params);    /////

        float filtered_param;

        // if we need to filter in the complex domain:
        if (filter_in_complex_domain) {
            arma::cx_fvec zparams = arma::exp(
                    arma::cx_fvec(arma::zeros<arma::fvec>(Nt), params));            // zparams = exp( i*params );
            arma::cx_fvec B = arma::solve(
                    arma::cx_fmat(arma::join_horiz(weights, weights % t_.head(Nt)), arma::zeros<arma::fmat>(Nt, 2)),
                    weights % zparams);
            filtered_param = std::arg(arma::as_scalar(B(0)));
        } else {
            arma::fvec B = arma::solve(arma::join_horiz(weights, weights % t_.head(Nt)), weights % params);
            filtered_param = arma::as_scalar(B(0));
        }

        //if ( exc==(weights_array.get_size(0)-1) && set==(weights_array.get_size(1)-1) &&
        //	 slc==(weights_array.get_size(2)-1) )
        //{
        //    write_nd_array< float >( &weights_array, "/tmp/nav_weights.real" );
        //    write_nd_array< float >( &nav_corr_param_array, "/tmp/nav_param_array.real" );
        //}
        //GDEBUG_STREAM("orig parameter: " << nav_corr_param_array(exc, set, slc) << "; filtered: " << filtered_param );

        return filtered_param;
    }


////////////////////////////////////////////////////
//
//  increase_no_repetitions
//
//    function to increase the size of the navigator parameter arrays used for filtering
//    - delta_rep: how many more repetitions to add

    void EPICorrGadget::increase_no_repetitions(size_t delta_rep) {

        GDEBUG_STREAM("cbi_EPICorrGadget WARNING: repetition number larger than what specified in header");

        size_t REP = Nav_mag_.get_size(0) / E2_;   // current maximum number of repetitions
        size_t new_REP = REP + delta_rep;
        size_t SET = Nav_mag_.get_size(1);
        size_t SLC = Nav_mag_.get_size(2);

        // create a new temporary array:
        hoNDArray<float> tmpArray(E2_ * new_REP, SET, SLC);
        tmpArray.fill(float(0.));

        // For each navigator parameter array, copy what we have so far to the temporary array, and then copy back:

        // Nav_mag_ :
        for (size_t slc = 0; slc < SLC; ++slc) {
            for (size_t set = 0; set < SET; ++set) {
                memcpy(&tmpArray(0, set, slc), &Nav_mag_(0, set, slc), Nav_mag_.get_number_of_bytes() / SET / SLC);
            }
        }
        Nav_mag_ = tmpArray;

        // B0_intercept_ :
        for (size_t slc = 0; slc < SLC; ++slc) {
            for (size_t set = 0; set < SET; ++set) {
                memcpy(&tmpArray(0, set, slc), &B0_intercept_(0, set, slc),
                       B0_intercept_.get_number_of_bytes() / SET / SLC);
            }
        }
        B0_intercept_ = tmpArray;

        // B0_slope_ :
        if (B0CorrectionMode.compare("linear") == 0) {
            for (size_t slc = 0; slc < SLC; ++slc) {
                for (size_t set = 0; set < SET; ++set) {
                    memcpy(&tmpArray(0, set, slc), &B0_slope_(0, set, slc),
                           B0_slope_.get_number_of_bytes() / SET / SLC);
                }
            }
            B0_slope_ = tmpArray;
        }

        // OE_phi_intercept_ :
        for (size_t slc = 0; slc < SLC; ++slc) {
            for (size_t set = 0; set < SET; ++set) {
                memcpy(&tmpArray(0, set, slc), &OE_phi_intercept_(0, set, slc),
                       OE_phi_intercept_.get_number_of_bytes() / SET / SLC);
            }
        }
        OE_phi_intercept_ = tmpArray;

        // OE_phi_slope_ :
        if ((OEPhaseCorrectionMode.compare("linear") == 0) ||
            (OEPhaseCorrectionMode.compare("polynomial") == 0)) {
            for (size_t slc = 0; slc < SLC; ++slc) {
                for (size_t set = 0; set < SET; ++set) {
                    memcpy(&tmpArray(0, set, slc), &OE_phi_slope_(0, set, slc),
                           OE_phi_slope_.get_number_of_bytes() / SET / SLC);
                }
            }
            OE_phi_slope_ = tmpArray;

            // OE_phi_poly_coef_ :
            if (OEPhaseCorrectionMode.compare("polynomial") == 0) {
                for (size_t i = 0; i < OE_phi_poly_coef_.size(); ++i) {
                    for (size_t slc = 0; slc < SLC; ++slc) {
                        for (size_t set = 0; set < SET; ++set) {
                            memcpy(&tmpArray(0, set, slc), &OE_phi_poly_coef_[i](0, set, slc),
                                   OE_phi_poly_coef_[i].get_number_of_bytes() / SET / SLC);
                        }
                    }
                    OE_phi_poly_coef_[i] = tmpArray;
                }
            }
        }

    }

    GADGETRON_GADGET_EXPORT(EPICorrGadget)
}
