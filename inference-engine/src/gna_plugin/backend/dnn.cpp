// Copyright (C) 2018-2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <cstdio>
#include <cmath>

#include <details/ie_exception.hpp>

#if GNA_LIB_VER == 2
#include <gna2-model-api.h>
#include "gna2_model_helper.hpp"
#include "gna2_model_debug_log.hpp"
#else
#include <gna-api-types-xnn.h>

#endif

#ifndef _NO_MKL_
#include <mkl_dnn.h>
#endif

#include "runtime/floatmath.h"
#include "dnn.hpp"
#include "gna_plugin_log.hpp"
#include "runtime/pwl.h"
#include "runtime/cnn.h"


void GNAPluginNS::backend::ApplyAffineTransform(intel_dnn_component_t *component, uint32_t *list, uint32_t listsize) {
    if (4 != component->num_bytes_per_input) {
        THROW_GNA_EXCEPTION << "Bad data width: " << component->num_bytes_per_input;
    }

    auto transform = &component->op.affine;
    int m = component->num_rows_out;
    int n = component->num_columns_in;
    int k = component->num_rows_in;
    int lda = component->num_rows_in;
    int ldb = component->num_columns_in;
    int ldc = component->num_columns_out;

    auto A = reinterpret_cast<float *>(transform->ptr_weights);
    auto B = reinterpret_cast<float *>(component->ptr_inputs);
    auto C = reinterpret_cast<float *>(component->ptr_outputs);
    auto bias = reinterpret_cast<float *>(transform->ptr_biases);
    if (list == nullptr) {
        for (uint32_t i = 0; i < m; i++) {
            for (uint32_t j = 0; j < n; j++) {
                C[i * ldc + j] = bias[i];
            }
        }
        cblas_sgemm1(CblasRowMajor, CblasNoTrans, CblasNoTrans, m, n, k, 1.0, A, lda, B, ldb, 1.0, C, ldc);
    } else {
        for (int l = 0; l < listsize; l++) {
            int i = list[l];
            for (uint32_t j = 0; j < n; j++) {
                C[l * ldc + j] = bias[i];
            }
        }
        cblas_sgemm_subset(CblasRowMajor,
                           CblasNoTrans,
                           CblasNoTrans,
                           m,
                           n,
                           k,
                           1.0,
                           A,
                           lda,
                           B,
                           ldb,
                           1.0,
                           C,
                           ldc,
                           list,
                           listsize);
    }
}

void GNAPluginNS::backend::ApplyDiagonalTransform(intel_dnn_component_t *component) {
    if (4 != component->num_bytes_per_input) {
        THROW_GNA_EXCEPTION << "Bad data width: " << component->num_bytes_per_input;
    }

    auto transform = &component->op.affine;
    int m = component->num_rows_out;
    int n = component->num_columns_in;
    int ldb = component->num_columns_in;
    int ldc = component->num_columns_out;

    auto A = reinterpret_cast<float *>(transform->ptr_weights);
    auto B = reinterpret_cast<float *>(component->ptr_inputs);
    auto C = reinterpret_cast<float *>(component->ptr_outputs);
    auto bias = reinterpret_cast<float *>(transform->ptr_biases);
    for (uint32_t i = 0; i < m; i++) {
        for (uint32_t j = 0; j < n; j++) {
            C[i * ldc + j] = bias[i];
        }
    }
    for (uint32_t j = 0; j < n; j++) {
        float *Bcol = B + j * ldb;
        float *Ccol = C + j * ldc;
        cblas_ssbmv1(CblasRowMajor, CblasLower, m, 0, 1.0, A, 1, Bcol, 1, 1.0, Ccol, 1);
    }
}

void GNAPluginNS::backend::ApplyRecurrentTransform(intel_dnn_component_t *component, uint32_t row, void *ptr_feedbacks) {
    if (4 != component->num_bytes_per_input) {
        THROW_GNA_EXCEPTION << "Bad data width: " << component->num_bytes_per_input;
    }

    intel_recurrent_t *transform = &component->op.recurrent;
    int k1 = component->num_columns_in;
    int k2 = component->num_columns_out;
    int n = k2;

    if (component->op.recurrent.ptr_feedbacks == nullptr) {
        THROW_GNA_EXCEPTION << "nullptr feedback pointer";
    }
    auto A1 = reinterpret_cast<float *>(component->ptr_inputs) + row * component->num_columns_in;
    auto A2 = reinterpret_cast<float *>(ptr_feedbacks);
    auto X = reinterpret_cast<float *>(transform->ptr_weights);
    auto B = reinterpret_cast<float *>(transform->ptr_biases);
    auto C = reinterpret_cast<float *>(component->ptr_outputs) + row * component->num_columns_out;
    sgemv_split(n, k1, k2, A1, A2, X, B, C);
}

void GNAPluginNS::backend::ApplyConvolutional1DTransform(intel_dnn_component_t *component) {
    if (4 != component->num_bytes_per_input) {
        THROW_GNA_EXCEPTION << "Bad data width: " << component->num_bytes_per_input;
    }
    CNNFilter32(component);
}

void GNAPluginNS::backend::ApplyPiecewiseLinearTransform(intel_dnn_component_t *component,
                                            intel_dnn_number_type_t number_type,
                                            uint32_t listsize) {
    if (kDnnFloat != number_type) {
        THROW_GNA_EXCEPTION << "Bad number type: " << number_type;
    }
    PwlApply32(component, listsize);
}

void GNAPluginNS::backend::ApplyPiecewiseLinearTransform(intel_dnn_component_t *component,
                                            intel_dnn_number_type_t number_type,
                                            uint32_t listsize,
                                            uint32_t num_row) {
    if (kDnnFloat != number_type) {
        THROW_GNA_EXCEPTION << "Bad number type: " << number_type;
    }
    PwlApply32(component, num_row, num_row, 0, listsize - 1);
}

void GNAPluginNS::backend::ApplyMaxPoolTransform(intel_dnn_component_t *component, intel_dnn_number_type_t number_type) {
    if (4 != component->num_bytes_per_input) {
        THROW_GNA_EXCEPTION << "Bad data width: " << component->num_bytes_per_input;
    }
    CNNMaxPool(component, number_type);
}

void GNAPluginNS::backend::ApplyTranspose(intel_dnn_component_t *component) {
    if (4 != component->num_bytes_per_input) {
        THROW_GNA_EXCEPTION << "Bad data width: " << component->num_bytes_per_input;
    }

    int m = component->num_rows_in;
    int n = component->num_columns_in;
    int lda = component->num_columns_in;
    int ldb = component->num_columns_out;
    // B = Transpose(A) where A is mxn and B is nxm
    auto A = reinterpret_cast<float *>(component->ptr_inputs);
    auto B = reinterpret_cast<float *>(component->ptr_outputs);
    for (uint32_t row = 0; row < m; row++) {
        for (uint32_t col = 0; col < n; col++) {
            B[col * ldb + row] = A[row * lda + col];
        }
    }
}

void GNAPluginNS::backend::ApplyCopy(intel_dnn_component_t *component) {
    if (4 != component->num_bytes_per_input) {
        THROW_GNA_EXCEPTION << "Bad data width: " << component->num_bytes_per_input;
    }

    auto src = reinterpret_cast<uint8_t *>(component->ptr_inputs);
    auto dst = reinterpret_cast<uint8_t *>(component->ptr_outputs);
    int32_t m = component->op.copy.num_copy_rows;
    int32_t n = component->op.copy.num_copy_columns;
    int32_t lda = component->num_columns_in;
    int32_t ldb = component->num_columns_out;
    if (m > component->num_rows_in) {
        THROW_GNA_EXCEPTION << "Error:  attempt to copy more columns than matrix has";
    }
    auto A = reinterpret_cast<float *>(src);
    auto B = reinterpret_cast<float *>(dst);
    for (uint32_t row = 0; row < m; row++) {
        for (uint32_t col = 0; col < n; col++) {
            B[row * ldb + col] = A[row * lda + col];
        }
    }
}

bool GNAPluginNS::backend::isCompatibleDnn(GNAPluginNS::backend::AMIntelDNN dnn1, GNAPluginNS::backend::AMIntelDNN dnn2) {
    bool isCompatible = true;

    // compare basic structures to see if they are compatible
    if (dnn1.num_components() != dnn2.num_components()) isCompatible = false;
    for (int i = 0; i < dnn1.num_components(); i++) {
        if (dnn1.component[i].num_rows_in != dnn2.component[i].num_rows_in) isCompatible = false;
        if (dnn1.component[i].num_columns_in != dnn2.component[i].num_columns_in) isCompatible = false;
        if (dnn1.component[i].num_rows_out != dnn2.component[i].num_rows_out) isCompatible = false;
        if (dnn1.component[i].num_columns_out != dnn2.component[i].num_columns_out) isCompatible = false;
        if (dnn1.component[i].operation != dnn2.component[i].operation) isCompatible = false;
    }

    return (isCompatible);
}

void GNAPluginNS::backend::ClearScoreError(intel_score_error_t *error) {
    error->num_scores = 0;
    error->num_errors = 0;
    error->max_error = 0.0;
    error->sum_error = 0.0;
    error->sum_squared_error = 0.0;
    error->max_rel_error = 0.0;
    error->sum_rel_error = 0.0;
    error->sum_squared_rel_error = 0.0;
}

void GNAPluginNS::backend::UpdateScoreError(intel_score_error_t *error, intel_score_error_t *total_error) {
    total_error->num_errors += error->num_errors;
    total_error->num_scores += error->num_scores;
    total_error->sum_error += error->sum_error;
    total_error->sum_squared_error += error->sum_squared_error;
    if (error->max_error > total_error->max_error) {
        total_error->max_error = error->max_error;
    }
    total_error->sum_rel_error += error->sum_rel_error;
    total_error->sum_squared_rel_error += error->sum_squared_rel_error;
    if (error->max_rel_error > total_error->max_rel_error) {
        total_error->max_rel_error = error->max_rel_error;
    }
}

void GNAPluginNS::backend::SoftmaxGoogle(float *ptr_output, float *ptr_input, const uint32_t num_outputs, const uint32_t num_inputs) {
    // Assumes input vector contains log likelihoods
    // The computes x[i] = x[i] - log(sum_j exp(x[j]))
    // This normalizes the likelihoods by the sum of likelihoods but stores them as log likelihoods

    float max_score = ptr_input[0];
    float sum = 0.0;
    float diff;
    // find max score for normalization to [0,1]
    for (uint32_t i = 0; i < num_inputs; i++) {
        if (ptr_input[i] > max_score) {
            max_score = ptr_input[i];
        }
    }
    for (uint32_t i = 0; i < num_inputs; i++) {
        sum += exp(ptr_input[i] - max_score);
    }
    if (sum < 1.0e-20) {
        fprintf(stderr, "Warning:  attempt to take log(0) in SoftmaxGoogle()!\n");
        sum = 1.0e-20;
    }
    diff = max_score + log(sum);
    for (uint32_t i = 0; i < num_outputs; i++) {
        ptr_output[i] = ptr_input[i] - diff;
    }
}
