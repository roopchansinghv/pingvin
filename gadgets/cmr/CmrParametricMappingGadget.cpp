
#include "CmrParametricMappingGadget.h"
#include <iomanip>
#include <sstream>

#include "hoNDArray_reductions.h"
#include "mri_core_def.h"
#include "hoNDArray_utils.h"
#include "hoNDArray_elemwise.h"
#include "mri_core_utility.h"
#include "morphology.h"

namespace Gadgetron {

    CmrParametricMappingGadget::CmrParametricMappingGadget(const Core::Context &context, const Core::GadgetProperties &properties)
        : BaseClass(context, properties)
    {
        auto& h = context.header;

        if (!h.acquisition_system_information)
        {
            GADGET_THROW("acquisitionSystemInformation not found in header. Bailing out");
        }

        // -------------------------------------------------

        float field_strength_T_ = h.acquisition_system_information->system_field_strength_t.value_or(1.5);
        GDEBUG_CONDITION_STREAM(verbose, "field_strength_T_ is read from protocol : " << field_strength_T_);

        if (h.encoding.size() != 1)
        {
            GDEBUG("Number of encoding spaces: %d\n", h.encoding.size());
        }

        size_t e = 0;
        mrd::EncodingSpaceType e_space = h.encoding[e].encoded_space;
        mrd::EncodingSpaceType r_space = h.encoding[e].recon_space;
        mrd::EncodingLimitsType e_limits = h.encoding[e].encoding_limits;

        meas_max_idx_.kspace_encode_step_1 = (uint16_t)e_space.matrix_size.y - 1;
        meas_max_idx_.set = (e_limits.set && (e_limits.set->maximum>0)) ? e_limits.set->maximum : 0;
        meas_max_idx_.phase = (e_limits.phase && (e_limits.phase->maximum>0)) ? e_limits.phase->maximum : 0;
        meas_max_idx_.kspace_encode_step_2 = (uint16_t)e_space.matrix_size.z - 1;
        meas_max_idx_.contrast = (e_limits.contrast && (e_limits.contrast->maximum > 0)) ? e_limits.contrast->maximum : 0;
        meas_max_idx_.slice = (e_limits.slice && (e_limits.slice->maximum > 0)) ? e_limits.slice->maximum : 0;
        meas_max_idx_.repetition = e_limits.repetition ? e_limits.repetition->maximum : 0;
        meas_max_idx_.average = e_limits.average ? e_limits.average->maximum : 0;
        meas_max_idx_.segment = 0;
    }

    void CmrParametricMappingGadget::process(Core::InputChannel<mrd::ImageArray>& in, Core::OutputChannel& out)
    {
        for (auto m1: in) {
            if (perform_timing) { gt_timer_local_.start("CmrParametricMappingGadget::process"); }

            GDEBUG_CONDITION_STREAM(verbose, "CmrParametricMappingGadget::process(...) starts ... ");

            // -------------------------------------------------------------

            process_called_times_++;

            // -------------------------------------------------------------

            mrd::ImageArray* data = &m1;

            // print out data info
            if (verbose)
            {
                GDEBUG_STREAM("----> CmrParametricMappingGadget::process(...) has been called " << process_called_times_ << " times ...");
                std::stringstream os;
                data->data.print(os);
                GDEBUG_STREAM(os.str());
            }

            // -------------------------------------------------------------

            // some images do not need mapping
            if (data->meta[0].count(skip_processing_meta_field) > 0)
            {
                out.push(std::move(m1));
                continue;
            }

            // -------------------------------------------------------------

            size_t encoding = (size_t)std::get<long>(data->meta[0]["encoding"].front());
            GADGET_CHECK_THROW(encoding < num_encoding_spaces_);

            std::string dataRole = std::get<std::string>(data->meta[0][GADGETRON_DATA_ROLE].front());

            size_t RO = data->data.get_size(0);
            size_t E1 = data->data.get_size(1);
            size_t E2 = data->data.get_size(2);
            size_t CHA = data->data.get_size(3);
            size_t N = data->data.get_size(4);
            size_t S = data->data.get_size(5);
            size_t SLC = data->data.get_size(6);

            std::stringstream os;
            os << "_encoding_" << encoding << "_processing_" << process_called_times_;
            std::string str = os.str();

            // -------------------------------------------------------------

            if (!debug_folder_full_path_.empty())
            {
                gt_exporter_.export_array_complex(data->data, debug_folder_full_path_ + "data" + str);
            }

            // -------------------------------------------------------------
            // if prep times are not read from protocol, find them from image header
            if (!imaging_prep_time_from_protocol)
            {
                this->prep_times_.resize(N, 0);

                size_t n;
                for (n = 0; n < N; n++)
                {
                    this->prep_times_[n] = data->headers(n).user_int[7] * 1e-3; // convert microsecond to ms
                }

                if (this->verbose)
                {
                    GDEBUG_STREAM("Prep times are read from images ... ");
                    for (n = 0; n < N; n++)
                    {
                        GDEBUG_STREAM("Image " << n << " - " << this->prep_times_[n] << " ms ");
                    }
                }
            }

            // -------------------------------------------------------------

            // calling the mapping
            mrd::ImageArray map, para, map_sd, para_sd;

            int status = this->perform_mapping(*data, map, para, map_sd, para_sd);

            if (status != GADGET_OK)
            {
                GWARN("CmrParametricMappingGadget::process, process incoming data failed ... ");

                // sending the incoming images
                out.push(std::move(m1));
            }
            else
            {
                if (!debug_folder_full_path_.empty())
                {
                    gt_exporter_.export_array_complex(map.data, debug_folder_full_path_ + "map" + str);
                    gt_exporter_.export_array_complex(para.data, debug_folder_full_path_ + "para" + str);
                    gt_exporter_.export_array_complex(map_sd.data, debug_folder_full_path_ + "map_sd" + str);
                    gt_exporter_.export_array_complex(para_sd.data, debug_folder_full_path_ + "para_sd" + str);
                }

                // sending the incoming images
                out.push(std::move(m1));

                // ----------------------------------------------------------
                // fill in image header and meta
                // send out results
                // ----------------------------------------------------------
                if ( send_sd_map && (this->fill_sd_header(map_sd) == GADGET_OK) )
                {
                    out.push(std::move(map_sd));
                }

                if (send_map && (this->fill_map_header(map) == GADGET_OK))
                {
                    out.push(std::move(map));
                }
            }

            if (perform_timing) { gt_timer_local_.stop(); }
        }
    }

    int CmrParametricMappingGadget::fill_map_header(mrd::ImageArray& map)
    {
        try
        {
            size_t RO  = map.data.get_size(0);
            size_t E1  = map.data.get_size(1);
            size_t E2  = map.data.get_size(2);
            size_t CHA = map.data.get_size(3);
            size_t N   = map.data.get_size(4);
            size_t S   = map.data.get_size(5);
            size_t SLC = map.data.get_size(6);

            size_t e2, cha, n, s, slc;

            std::string lut = color_lut_map;
            if (this->field_strength_T_ > 2)
            {
                lut = color_lut_map_3T;
            }

            std::ostringstream ostr;
            ostr << "x" << (double)scaling_factor_map;
            std::string scalingStr = ostr.str();

            std::ostringstream ostr_unit;
            ostr_unit << std::setprecision(3) << 1.0f / scaling_factor_map << "ms";
            std::string unitStr = ostr_unit.str();

            for (slc = 0; slc < SLC; slc++)
            {
                for (s = 0; s < S; s++)
                {
                    for (n = 0; n < N; n++)
                    {
                        auto& meta = map.meta(n, s, slc);
                        meta[GADGETRON_IMAGE_SCALE_RATIO] = {(double)scaling_factor_map};
                        meta[GADGETRON_IMAGE_WINDOWCENTER] = {(long)(window_center_map*scaling_factor_map)};
                        meta[GADGETRON_IMAGE_WINDOWWIDTH] = {(long)(window_width_map*scaling_factor_map)};
                        meta[GADGETRON_IMAGE_COLORMAP] = {lut};

                        meta[GADGETRON_IMAGECOMMENT] = meta[GADGETRON_DATA_ROLE];
                        meta[GADGETRON_IMAGECOMMENT].push_back(scalingStr);
                        meta[GADGETRON_IMAGECOMMENT].push_back(unitStr);
                    }
                }
            }

            Gadgetron::scal( (float)(scaling_factor_map), map.data);
        }
        catch (...)
        {
            GERROR_STREAM("Exceptions happened in CmrParametricMappingGadget::fill_map_header(...) ... ");
            return GADGET_FAIL;
        }

        return GADGET_OK;
    }

    int CmrParametricMappingGadget::fill_sd_header(mrd::ImageArray& map_sd)
    {
        try
        {
            size_t RO = map_sd.data.get_size(0);
            size_t E1 = map_sd.data.get_size(1);
            size_t E2 = map_sd.data.get_size(2);
            size_t CHA = map_sd.data.get_size(3);
            size_t N = map_sd.data.get_size(4);
            size_t S = map_sd.data.get_size(5);
            size_t SLC = map_sd.data.get_size(6);

            std::string lut = color_lut_sd_map;

            std::ostringstream ostr;
            ostr << "x" << (double)scaling_factor_sd_map;
            std::string scalingStr = ostr.str();

            std::ostringstream ostr_unit;
            ostr_unit << std::setprecision(3) << 1.0f / scaling_factor_sd_map << "ms";
            std::string unitStr = ostr_unit.str();

            size_t e2, cha, n, s, slc;

            for (slc = 0; slc < SLC; slc++)
            {
                for (s = 0; s < S; s++)
                {
                    for (n = 0; n < N; n++)
                    {
                        auto& meta = map_sd.meta(n, s, slc);
                        meta[GADGETRON_IMAGE_SCALE_RATIO] = {(double)scaling_factor_sd_map};
                        meta[GADGETRON_IMAGE_WINDOWCENTER] = {(long)(window_center_sd_map*scaling_factor_sd_map)};
                        meta[GADGETRON_IMAGE_WINDOWWIDTH] = {(long)(window_width_sd_map*scaling_factor_sd_map)};
                        meta[GADGETRON_IMAGE_COLORMAP] = {lut};

                        meta[GADGETRON_IMAGECOMMENT] = meta[GADGETRON_DATA_ROLE];
                        meta[GADGETRON_IMAGECOMMENT].push_back(scalingStr);
                        meta[GADGETRON_IMAGECOMMENT].push_back(unitStr);
                    }
                }
            }

            Gadgetron::scal((float)(scaling_factor_map), map_sd.data);
        }
        catch (...)
        {
            GERROR_STREAM("Exceptions happened in CmrParametricMappingGadget::fill_sd_header(...) ... ");
            return GADGET_FAIL;
        }

        return GADGET_OK;
    }

    void CmrParametricMappingGadget::compute_mask_for_mapping(const hoNDArray<float> &mag, hoNDArray<float> &mask,
                                                              float scale_factor)
    {
        size_t RO = mag.get_size(0);
        size_t E1 = mag.get_size(1);
        size_t SLC = mag.get_size(2);

        hoNDArray<float> mask_initial;
        mask_initial.create(RO, E1, SLC);
        Gadgetron::fill(mask_initial, (float)1);

        double std_thres = std_thres_masking;

        size_t n;
        for (n = 0; n < RO*E1*SLC; n++)
        {
            if (mag(n) < scale_factor*std_thres)
            {
                mask_initial(n) = 0;
            }
        }

        mask = mask_initial;

        size_t obj_thres = 20;
        size_t bg_thres = 20;
        bool is_8_connected = true;

        size_t slc;
        for (slc = 0; slc < SLC; slc++)
        {
            hoNDArray<float> mask2D;
            mask2D.create(RO, E1, &mask_initial(0, 0, slc));

            hoNDArray<float> mask2D_clean;
            mask2D_clean.create(RO, E1, &mask(0, 0, slc));

            Gadgetron::bwlabel_clean_fore_and_background(mask2D, (float)1, (float)0, obj_thres, bg_thres, is_8_connected, mask2D_clean);
        }


    }
}
