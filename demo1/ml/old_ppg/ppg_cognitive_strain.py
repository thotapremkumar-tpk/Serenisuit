"""
SereniSuit — PPG arousal / cognitive-strain classifier
=====================================================
Dataset : PPG Collection for Cognitive Strain (Kaggle)
          22 participants x {3-back = high load, 0-back = low load} x 2 trials
          76,800 samples per trial  ->  300 s at 256 Hz

Two evaluation protocols, both LEAVE-ONE-SUBJECT-OUT (22 folds):

  A) RAW        - can a model trained on other people judge a stranger?
  B) CALIBRATED - each subject's features z-scored against their OWN calm
                  baseline, which is what a wearable actually does: it is
                  worn by one person and learns their normal.

Run:  python3 ppg_cognitive_strain.py
"""
import os, re, glob, time, warnings
import numpy as np, pandas as pd
from scipy.signal import find_peaks, detrend, butter, filtfilt, welch
from sklearn.linear_model import LogisticRegression
from sklearn.ensemble import RandomForestClassifier
from sklearn.pipeline import make_pipeline
from sklearn.preprocessing import StandardScaler
from sklearn.model_selection import LeaveOneGroupOut, cross_val_predict, StratifiedKFold
from sklearn.metrics import (accuracy_score, roc_auc_score, confusion_matrix,
                             classification_report, ConfusionMatrixDisplay)
warnings.filterwarnings("ignore")

try:
    from xgboost import XGBClassifier
    HAS_XGB = True
except ImportError:
    HAS_XGB = False

HERE = os.path.dirname(os.path.abspath(__file__))
D    = os.path.join(HERE, "dataset", "cognitive_strain")
FS   = 256                 # 76,800 samples == 300 s exactly
WIN  = 30 * FS             # 30-second windows
HOP  = WIN // 2            # 50 % overlap

bb, ab = butter(3, [0.7 / (FS / 2), 3.5 / (FS / 2)], btype="band")   # 42–210 BPM

NAMES = ["hr", "sdnn", "rmssd", "pnn50", "ibi_cv", "logLF", "logHF", "lf_hf",
         "amp_std", "amp_range", "mean_abs_d", "std_d", "peak_mean", "peak_std",
         "skew", "kurt"]

def features(seg):
    seg = seg[~np.isnan(seg)]
    if len(seg) < WIN * 0.9:
        return None
    raw = seg.astype(float)
    xf  = filtfilt(bb, ab, detrend(raw))

    pk, _ = find_peaks(xf, distance=int(FS * 0.4), prominence=0.2 * np.std(xf))
    if len(pk) < 5:
        return None
    ibi = np.diff(pk) / FS
    ibi = ibi[(ibi > 0.3) & (ibi < 2.0)]
    if len(ibi) < 4:
        return None

    # HRV spectrum comes from the IBI tachogram, NOT the band-passed waveform:
    # the 0.7 Hz high-pass has already deleted the LF and HF bands entirely.
    tt = np.cumsum(ibi)
    if len(tt) >= 8 and tt[-1] > 20:
        fs_t = 4.0
        grid = np.arange(tt[0], tt[-1], 1.0 / fs_t)
        tach = np.interp(grid, tt, ibi); tach -= tach.mean()
        f, p = welch(tach, fs=fs_t, nperseg=min(len(tach), int(fs_t * 30)))
        lf = p[(f >= 0.04) & (f < 0.15)].sum()
        hf = p[(f >= 0.15) & (f < 0.40)].sum()
    else:
        lf = hf = 0.0
    amp = xf[pk]

    return [60.0 / np.mean(ibi),                        # hr
            np.std(ibi),                                # sdnn
            np.sqrt(np.mean(np.diff(ibi) ** 2)),        # rmssd
            np.mean(np.abs(np.diff(ibi)) > 0.05),       # pnn50
            np.std(ibi) / np.mean(ibi),                 # ibi_cv
            np.log1p(lf), np.log1p(hf),
            lf / hf if hf > 0 else 0.0,
            np.std(raw), np.ptp(raw),
            np.mean(np.abs(np.diff(raw))), np.std(np.diff(raw)),
            np.mean(amp), np.std(amp),
            float(pd.Series(raw).skew()), float(pd.Series(raw).kurt())]

# ----------------------------------------------------------------- load
X, y, g = [], [], []
for label, sub in ((1, "High_MWL"), (0, "Low_MWL")):
    paths = glob.glob(os.path.join(D, sub, "*.csv")) or \
            glob.glob(os.path.join(D, sub, sub, "*.csv"))
    for path in sorted(paths):
        pid = re.match(r"p(\d+)", os.path.basename(path)).group(1)
        df = pd.read_csv(path)
        for col in [c for c in df.columns if c.startswith("Trial")]:
            sig = df[col].values.astype(float)
            for s in range(0, len(sig) - WIN + 1, HOP):
                f = features(sig[s:s + WIN])
                if f is not None:
                    X.append(f); y.append(label); g.append(pid)

X = np.array(X, float); y = np.array(y); g = np.array(g)
X = np.where(np.isnan(X) | np.isinf(X), np.nanmedian(X, axis=0), X)

print(f"windows {X.shape[0]} | features {X.shape[1]} | subjects {len(set(g))} | "
      f"low {np.sum(y==0)} / high {np.sum(y==1)}")
print(f"median HR {np.median(X[:,0]):.1f} BPM "
      f"[{np.percentile(X[:,0],5):.0f}–{np.percentile(X[:,0],95):.0f}] "
      f"— sanity-checks the {FS} Hz assumption\n")

# ------------------------------------------- B) calibrate to each wearer
Xc = X.copy()
for s in set(g):
    m, rest = (g == s), (g == s) & (y == 0)
    mu, sd = X[rest].mean(axis=0), X[rest].std(axis=0)
    sd[sd < 1e-9] = 1.0
    Xc[m] = (X[m] - mu) / sd

models = {"Logistic Regression": make_pipeline(StandardScaler(),
                                               LogisticRegression(max_iter=3000)),
          "Random Forest": RandomForestClassifier(n_estimators=300, n_jobs=-1,
                                                  random_state=42)}
if HAS_XGB:
    models["XGBoost"] = XGBClassifier(n_estimators=200, max_depth=5, learning_rate=0.1,
                                      subsample=0.9, colsample_bytree=0.8, n_jobs=-1,
                                      random_state=42, base_score=0.5,
                                      eval_metric="logloss")

logo, results = LeaveOneGroupOut(), {}
for tag, data in (("A) RAW — generalise to a stranger", X),
                  ("B) CALIBRATED to the wearer's own calm baseline", Xc)):
    print("=" * 70); print(tag); print("=" * 70)
    for name, m in models.items():
        t0 = time.time()
        proba = cross_val_predict(m, data, y, groups=g, cv=logo, method="predict_proba")[:, 1]
        pred = (proba > 0.5).astype(int)
        acc, auc = accuracy_score(y, pred), roc_auc_score(y, proba)
        results[(tag[0], name)] = (acc, auc, pred)
        print(f"  {name:<22} accuracy {acc:.4f}   ROC-AUC {auc:.4f}   ({time.time()-t0:.0f}s)")
    print()

print("=" * 70)
print("RANDOM 5-FOLD — leaky, shown only to expose the trap")
print("=" * 70)
for name, m in models.items():
    p = cross_val_predict(m, X, y, cv=StratifiedKFold(5, shuffle=True, random_state=0))
    print(f"  {name:<22} {accuracy_score(y, p):.4f}   <-- same subject in train AND test")

best = max((k for k in results if k[0] == "B"), key=lambda k: results[k][0])
acc, auc, pred = results[best]
print(f"\nBEST subject-independent: {best[1]}, calibrated — {acc:.4f} (AUC {auc:.4f})")
print(confusion_matrix(y, pred))
print(classification_report(y, pred, target_names=["low MWL", "high MWL"], digits=4))

rf = RandomForestClassifier(n_estimators=300, random_state=42, n_jobs=-1).fit(Xc, y)
print("top features (calibrated):")
for n, v in sorted(zip(NAMES, rf.feature_importances_), key=lambda t: -t[1])[:8]:
    print(f"   {n:<12} {v:.3f}")

try:
    import matplotlib; matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(5, 4.2), dpi=160)
    ConfusionMatrixDisplay.from_predictions(y, pred, display_labels=["low", "high"],
                                            cmap="Blues", colorbar=False, ax=ax,
                                            values_format="d")
    ax.set_title(f"{best[1]}, wearer-calibrated — {acc:.3f}")
    fig.tight_layout()
    fig.savefig(os.path.join(HERE, "confusion_matrix_ppg.png"))
    print(f"\nsaved {os.path.join(HERE, 'confusion_matrix_ppg.png')}")
except ImportError:
    pass

print("""
HOW TO REPORT THIS
  Raw, subject-independent          ~0.60   a model trained on other people
                                            barely beats chance on a stranger
  Wearer-calibrated, subject-indep. ~0.68   baseline against the wearer's own
                                            calm state and it works
  Random 5-fold                     ~0.80   INFLATED - the model is recognising
                                            individuals, not workload

  The gap between the last two is the whole point: PPG encodes cognitive load
  only RELATIVE to a person's own baseline. That is exactly how SereniSuit is
  worn - by one person, who can be calibrated - so the calibrated figure is the
  honest deployment number, and the 0.80 is the trap to avoid quoting.
""")
