#ifndef PPG_MODEL_H
#define PPG_MODEL_H
#ifdef __cplusplus
extern "C" {
#endif

#define PPG_N_FEAT 16

/* 16 HRV/PPG features, each z-scored against the WEARER's own calm baseline.
   order: hr, sdnn, rmssd, pnn50, ibi_cv, logLF, logHF, lf_hf, amp_std, amp_range, mean|d|, std_d, peak_mean, peak_std, skew, kurt
   returns P(high arousal). LOSO accuracy 0.6770. */
float ppg_arousal_score(const float *calibrated_feats);

#ifdef __cplusplus
}
#endif
#endif
