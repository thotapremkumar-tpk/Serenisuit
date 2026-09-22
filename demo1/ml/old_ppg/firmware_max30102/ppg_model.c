#include <math.h>
#include "ppg_model.h"

/* PPG arousal model - logistic regression on 16 wearer-calibrated
   features. Leave-one-subject-out accuracy 0.6770, ROC-AUC 0.7561.
   Trained on PPG Collection for Cognitive Strain, 22 participants. */

#define N_FEAT 16

static const float FEAT_MEAN[N_FEAT] = {0.473788f, -0.152234f, -0.115764f, -0.205387f, -0.100248f, -0.044612f, 0.105736f, 0.043733f, -0.424630f, -0.451946f, -0.362005f, -0.401210f, -0.405898f, -0.273461f, -0.457045f, -0.145108f};
static const float FEAT_SCALE[N_FEAT] = {1.224942f, 1.043000f, 1.488218f, 1.145255f, 1.036094f, 1.026111f, 4.604998f, 1.271514f, 1.240054f, 1.313682f, 1.160652f, 1.194763f, 1.186015f, 1.074517f, 1.343497f, 1.232181f};
static const float COEF[N_FEAT] = {1.081021f, -0.142123f, 0.399909f, -0.212242f, -0.294962f, 0.024072f, 0.991099f, 0.186681f, 0.246026f, -0.306041f, -0.582651f, -1.068879f, 1.455943f, -0.121326f, -0.528175f, -0.048596f};
static const float INTERCEPT = 0.125456f;

/* feature order: hr, sdnn, rmssd, pnn50, ibi_cv, logLF, logHF, lf_hf, amp_std, amp_range, mean|d|, std_d, peak_mean, peak_std, skew, kurt */
float ppg_arousal_score(const float *calibrated_feats) {
  float z = INTERCEPT;
  for (int i = 0; i < N_FEAT; i++)
    z += COEF[i] * ((calibrated_feats[i] - FEAT_MEAN[i]) / FEAT_SCALE[i]);
  return 1.0f / (1.0f + expf(-z));   /* P(high arousal) */
}
