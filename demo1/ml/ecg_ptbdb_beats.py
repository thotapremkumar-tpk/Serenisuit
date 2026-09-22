"""
SereniSuit - ECG heartbeat-shape classifier for the AD8232
==========================================================
Dataset : PTB Diagnostic ECG Database, preprocessed heartbeat CSVs
          (Kaggle "shayanfazeli/heartbeat", Kachuee et al. 2018)
          dataset/ptbdb/ptbdb_normal.csv    4,046 beats   label 0
          dataset/ptbdb/ptbdb_abnormal.csv 10,506 beats   label 1
          One row = one heartbeat, 187 samples at 125 Hz, scaled 0..1,
          starting at the R-peak, zero-padded. Column 188 = label.

Task    : "typical" vs "atypical" beat SHAPE. This is a research signal,
          not a diagnosis - the abnormal class in PTB is mostly recordings
          from patients with myocardial infarction and other conditions,
          and SereniSuit is not a medical device.

Model   : small neural network (MLP 187 -> 128 -> 32 -> 1), exported to
          plain C (~113 KB of weights, no ML library) for the ESP32-S3.
          At boot the suit scores 8 held-out beats stored in flash
          (self-test, shown on the dashboards). It never drives the alert.

TRANSFER CHECK - why it is NOT run on the live AD8232 signal:
  With neurokit2 installed, this script also cuts beats out of healthy /
  simulated normal ECG from OTHER sources with the same recipe the
  firmware would use live (extract_beats below) and reports how many the
  model calls "typical". Result: only 0-26 %. The model has learned what
  PTB's recordings look like, not a general "healthy beat". Using it live
  needs our own AD8232 recordings (normal beats from our wearers) to
  retrain / fine-tune on - Phase 2. The live pipeline exists in the
  firmware behind ENABLE_LIVE_BEAT_SHAPE 0.

LIMITATION (say it before anyone asks):
  Rows are individual beats with no patient IDs, and beats from one
  patient land in both train and test. The test accuracy is therefore
  optimistic for a NEW person. A patient-independent split needs the
  original PTB records from PhysioNet (they carry patient IDs).

Run:  python3 ecg_ptbdb_beats.py
Needs: numpy, pandas, scikit-learn, matplotlib (optional, for the figure)

Writes:
  ../firmware/include/ecg_model.h       model interface
  ../firmware/src/ecg_model.c           weights + inference in plain C
  ../firmware/include/ecg_test_beats.h  8 held-out beats for the self-test
  confusion_matrix_ecg.png              figure for the slides
"""
import os, time, json, warnings
import numpy as np, pandas as pd
from sklearn.model_selection import train_test_split, StratifiedKFold
from sklearn.neural_network import MLPClassifier
from sklearn.linear_model import LogisticRegression
from sklearn.ensemble import RandomForestClassifier
from sklearn.metrics import (accuracy_score, roc_auc_score, f1_score,
                             confusion_matrix, classification_report)
warnings.filterwarnings("ignore")

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "dataset", "ptbdb")
FW   = os.path.join(HERE, "..", "firmware")

FS_BEAT  = 125          # dataset sampling rate
BEAT_LEN = 187          # samples per beat (zero-padded)
HIDDEN   = (128, 32)
SEED     = 42

# --------------------------------------------------------------- live beats
def extract_beats(x125):
    """Cut beats from a 10-second, 125 Hz ECG window exactly like the dataset
    (Kachuee et al. 2018). The firmware runs the same steps in C:
      1. scale the window to 0..1
      2. R-peak candidates = local maxima above 0.9
      3. T = median R-R interval
      4. each beat = R-peak .. R-peak + 1.2*T, zero-padded to 187
    If the QRS points downwards (electrodes swapped) the window is flipped.
    """
    x = np.asarray(x125, dtype=float)
    x = x - np.median(x)
    if -x.min() > x.max():                  # inverted QRS
        x = -x
    lo, hi = x.min(), x.max()
    if hi - lo < 1e-9:
        return []
    x = (x - lo) / (hi - lo)
    d = np.diff(x)
    peaks = [i for i in range(1, len(x) - 1) if d[i - 1] > 0 and d[i] <= 0 and x[i] > 0.9]
    # keep one peak per beat (refractory 0.25 s)
    r = []
    for p in peaks:
        if not r or p - r[-1] > 0.25 * FS_BEAT:
            r.append(p)
        elif x[p] > x[r[-1]]:
            r[-1] = p
    if len(r) < 2:
        return []
    T = int(np.median(np.diff(r)))
    n = min(int(1.2 * T), BEAT_LEN)
    beats = []
    for p in r:
        if p + n <= len(x):
            b = np.zeros(BEAT_LEN)
            b[:n] = x[p:p + n]
            beats.append(b)
    return beats

# ------------------------------------------------------------------ export
def cf(v, digits=7):
    """C float literal that always has a decimal point (1 -> 1.0f)."""
    t = f"{float(v):.{digits}g}"
    if "." not in t and "e" not in t and "n" not in t:
        t += ".0"
    return t + "f"

def c_array(name, arr, per_line=8):
    flat = np.asarray(arr, dtype=np.float32).ravel()
    lines = []
    for i in range(0, len(flat), per_line):
        lines.append("  " + ", ".join(cf(v) for v in flat[i:i + per_line]))
    return f"static const float {name}[{len(flat)}] = {{\n" + ",\n".join(lines) + "\n};\n"

def export_c(clf, test_X, test_y, test_p, stats):
    sizes = [BEAT_LEN] + list(HIDDEN) + [1]
    n_params = sum(c.size for c in clf.coefs_) + sum(b.size for b in clf.intercepts_)
    with open(os.path.join(FW, "include", "ecg_model.h"), "w") as f:
        f.write(f"""/* SereniSuit - ECG beat-shape model (generated by ml/ecg_ptbdb_beats.py) */
#ifndef ECG_MODEL_H
#define ECG_MODEL_H
#ifdef __cplusplus
extern "C" {{
#endif

#define ECG_BEAT_LEN   {BEAT_LEN}      /* samples per beat, 125 Hz, 0..1, starts at the R-peak */
#define ECG_BEAT_FS    {FS_BEAT}
#define ECG_MODEL_PARAMS {n_params}

/* probability (0..1) that a beat's SHAPE looks like the dataset's "abnormal"
   class. Research signal only - not a diagnosis. */
float ecg_beat_abnormal_prob(const float *beat);

#ifdef __cplusplus
}}
#endif
#endif
""")
    body = [f"""/* SereniSuit - ECG beat-shape model, generated by ml/ecg_ptbdb_beats.py
 * Dataset: PTB Diagnostic ECG (Kaggle heartbeat CSVs), {stats['n_train']} training beats
 * MLP {' -> '.join(map(str, sizes))}, ReLU, sigmoid output, {n_params} weights (float32)
 * Test accuracy {stats['acc']:.4f}, ROC-AUC {stats['auc']:.4f} on {stats['n_test']} held-out beats
 * (random beat split - optimistic for a new person, see the script). */
#include <math.h>
#include "ecg_model.h"

"""]
    for li, (W, b) in enumerate(zip(clf.coefs_, clf.intercepts_)):
        body.append(f"/* layer {li + 1}: {W.shape[0]} -> {W.shape[1]}, weights stored [in][out] */\n")
        body.append(c_array(f"W{li + 1}", W))
        body.append(c_array(f"B{li + 1}", b))
        body.append("\n")
    body.append(f"""static void dense(const float *in, int n_in, const float *W, const float *B,
                  float *out, int n_out, int relu) {{
  for (int o = 0; o < n_out; o++) out[o] = B[o];
  for (int i = 0; i < n_in; i++) {{
    float v = in[i];
    if (v == 0.0f) continue;                    /* zero padding is common */
    const float *w = W + i * n_out;
    for (int o = 0; o < n_out; o++) out[o] += v * w[o];
  }}
  if (relu)
    for (int o = 0; o < n_out; o++) if (out[o] < 0.0f) out[o] = 0.0f;
}}

float ecg_beat_abnormal_prob(const float *beat) {{
  float h1[{HIDDEN[0]}], h2[{HIDDEN[1]}], z;
  dense(beat, {BEAT_LEN}, W1, B1, h1, {HIDDEN[0]}, 1);
  dense(h1, {HIDDEN[0]}, W2, B2, h2, {HIDDEN[1]}, 1);
  dense(h2, {HIDDEN[1]}, W3, B3, &z, 1, 0);
  return 1.0f / (1.0f + expf(-z));
}}
""")
    with open(os.path.join(FW, "src", "ecg_model.c"), "w") as f:
        f.write("".join(body))

    with open(os.path.join(FW, "include", "ecg_test_beats.h"), "w") as f:
        f.write(f"""/* 8 held-out PTB beats for the on-device self-test (never seen in training).
 * Picked at random (seed {SEED}) from the test split: 4 normal, 4 abnormal.
 * Generated by ml/ecg_ptbdb_beats.py */
#ifndef ECG_TEST_BEATS_H
#define ECG_TEST_BEATS_H

#define N_TEST_BEATS {len(test_y)}

static const int   test_beat_labels[N_TEST_BEATS] = {{{', '.join(str(int(v)) for v in test_y)}}};
/* probabilities the Python model gives - the C output must match */
static const float test_beat_expected[N_TEST_BEATS] = {{{', '.join(cf(v, 6) for v in test_p)}}};
static const float test_beats[N_TEST_BEATS][{BEAT_LEN}] = {{
""")
        for row in test_X:
            f.write("  {" + ", ".join(cf(v, 6) for v in row) + "},\n")
        f.write("};\n\n#endif\n")

# ---------------------------------------------------------- transfer check
def firmware_like_125hz(x250):
    """What the firmware does to the AD8232 signal before extract_beats:
    0.5 Hz high-pass (same constant), then average pairs 250 Hz -> 125 Hz."""
    y = np.zeros(len(x250)); px = py = 0.0
    for i, v in enumerate(x250):
        py = 0.98757 * (py + v - px); px = v; y[i] = py
    return y[:len(y) // 2 * 2].reshape(-1, 2).mean(1)

def transfer_check(clf):
    try:
        import neurokit2 as nk
        from scipy.signal import resample_poly
    except ImportError:
        print("\n(transfer check skipped - pip install neurokit2 to run it)")
        return
    sources = {"real ECG (neurokit ecg_1000hz)": (nk.data("ecg_1000hz"), 1000),
               "real resting ECG, 5 min": (nk.data("bio_resting_5min_100hz")["ECG"].values, 100)}
    for hr in (60, 75, 90):
        sources[f"simulated ECG {hr} BPM (ECGSYN)"] = (
            nk.ecg_simulate(duration=60, sampling_rate=500, heart_rate=hr, noise=0.02,
                            method="ecgsyn", random_state=hr), 500)
    print("\nTRANSFER CHECK - normal ECG from other sources, same recipe as the firmware:")
    for name, (x, fs) in sources.items():
        y = firmware_like_125hz(resample_poly(x, 250, fs))[250:]
        beats = []
        for s in range(0, len(y) - 1250 + 1, 1250):
            beats += extract_beats(y[s:s + 1250])
        p = clf.predict_proba(np.array(beats, dtype=np.float32))[:, 1] if beats else np.array([])
        print(f"  {name:<36} {len(beats):4d} beats  {100 * np.mean(p < 0.5) if len(p) else 0:5.1f} % called typical")
    print("  -> it does not generalise beyond PTB's recordings: not used on the live signal.")

# -------------------------------------------------------------------- main
def main():
    n = pd.read_csv(os.path.join(DATA, "ptbdb_normal.csv"), header=None).values
    a = pd.read_csv(os.path.join(DATA, "ptbdb_abnormal.csv"), header=None).values
    D = np.vstack([n, a])
    X = D[:, :BEAT_LEN].astype(np.float32)
    y = D[:, BEAT_LEN].astype(int)
    print(f"beats {len(y)}   normal {np.sum(y == 0)}   abnormal {np.sum(y == 1)}")

    # same split as the earlier ecg_ptbdb_train.py -> comparable numbers
    Xtr, Xte, ytr, yte = train_test_split(X, y, test_size=0.2, stratify=y, random_state=SEED)

    def mlp(seed=0):
        return MLPClassifier(HIDDEN, alpha=1e-4, max_iter=600, random_state=seed,
                             early_stopping=True, n_iter_no_change=20)

    # 5-fold cross-validation on the training part: how stable is it?
    cv = []
    for k, (i_tr, i_va) in enumerate(StratifiedKFold(5, shuffle=True, random_state=SEED).split(Xtr, ytr)):
        m = mlp(k).fit(Xtr[i_tr], ytr[i_tr])
        p = m.predict_proba(Xtr[i_va])[:, 1]
        cv.append((accuracy_score(ytr[i_va], p > 0.5), roc_auc_score(ytr[i_va], p)))
    cv = np.array(cv)
    print(f"\n5-fold CV (train part): accuracy {cv[:, 0].mean():.4f} +/- {cv[:, 0].std():.4f}   "
          f"ROC-AUC {cv[:, 1].mean():.4f}")

    print("\nheld-out test split (20 %):")
    print(f"{'model':<34}{'accuracy':>9}{'ROC-AUC':>9}{'macro-F1':>10}{'size':>12}")
    ref = [("Logistic regression (reference)", LogisticRegression(max_iter=2000), "1 KB"),
           ("Random forest 200 (reference)", RandomForestClassifier(200, n_jobs=-1, random_state=0), "too big for MCU")]
    for name, clf, size in ref:
        clf.fit(Xtr, ytr); p = clf.predict_proba(Xte)[:, 1]
        print(f"{name:<34}{accuracy_score(yte, p > 0.5):>9.4f}{roc_auc_score(yte, p):>9.4f}"
              f"{f1_score(yte, p > 0.5, average='macro'):>10.4f}{size:>16}")

    t0 = time.time()
    clf = mlp(0).fit(Xtr, ytr)
    p = clf.predict_proba(Xte)[:, 1]
    pred = (p > 0.5).astype(int)
    n_params = sum(c.size for c in clf.coefs_) + sum(b.size for b in clf.intercepts_)
    acc, auc, f1 = accuracy_score(yte, pred), roc_auc_score(yte, p), f1_score(yte, pred, average="macro")
    print(f"{'MLP ' + str(HIDDEN) + ' -> ESP32':<34}{acc:>9.4f}{auc:>9.4f}{f1:>10.4f}"
          f"{f'{n_params * 4 / 1024:.0f} KB':>16}   ({time.time() - t0:.0f}s to train)")
    cm = confusion_matrix(yte, pred)
    print("\n             pred normal  pred abnormal")
    print(f"  normal      {cm[0, 0]:10d}  {cm[0, 1]:12d}")
    print(f"  abnormal    {cm[1, 0]:10d}  {cm[1, 1]:12d}\n")
    print(classification_report(yte, pred, target_names=["normal", "abnormal"], digits=4))

    # 8 held-out beats for the on-device self-test
    rng = np.random.default_rng(SEED)
    pick = np.concatenate([rng.choice(np.where(yte == c)[0], 4, replace=False) for c in (0, 1)])
    tX, ty = Xte[pick], yte[pick]
    tp = clf.predict_proba(tX)[:, 1]
    print("self-test beats (true -> P(abnormal)):", ", ".join(f"{t}->{q:.3f}" for t, q in zip(ty, tp)))

    stats = dict(acc=acc, auc=auc, f1=f1, n_train=len(ytr), n_test=len(yte),
                 cv_acc=float(cv[:, 0].mean()), cv_acc_std=float(cv[:, 0].std()), params=int(n_params))
    export_c(clf, tX, ty, tp, stats)
    with open(os.path.join(HERE, "ecg_model_metrics.json"), "w") as f:
        json.dump(stats, f, indent=2)
    print(f"\nwrote firmware/include/ecg_model.h, firmware/src/ecg_model.c, "
          f"firmware/include/ecg_test_beats.h, ml/ecg_model_metrics.json")

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        from sklearn.metrics import ConfusionMatrixDisplay
        fig, ax = plt.subplots(figsize=(5, 4.2), dpi=160)
        ConfusionMatrixDisplay.from_predictions(yte, pred, display_labels=["normal", "abnormal"],
                                                cmap="Blues", colorbar=False, ax=ax, values_format="d")
        ax.set_title(f"ECG beat MLP (on ESP32) - acc {acc:.3f}, AUC {auc:.3f}", fontsize=10)
        fig.tight_layout()
        fig.savefig(os.path.join(HERE, "confusion_matrix_ecg.png"))
        print("wrote ml/confusion_matrix_ecg.png")
    except ImportError:
        print("(matplotlib not installed - skipped the figure)")

    transfer_check(clf)

    print("""
LIMITATION: beats from one patient appear in both train and test (the CSVs
have no patient IDs), so these numbers are optimistic for a new person.
The suit only self-tests the model at boot - alerts come from the
60-100 BPM threshold on the live heart rate.""")

if __name__ == "__main__":
    main()
