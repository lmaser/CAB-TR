"""
CAB-TR MPT Verification Script
Compares our C++ MPT output against SciPy's scipy.signal.minimum_phase.

Usage:
  1. Export an IR from CAB-TR with MPT enabled (generates Desktop WAVs)
  2. Run: python verify_mpt.py

Requires: numpy, scipy, soundfile (pip install numpy scipy soundfile)
"""
import os
import numpy as np
import soundfile as sf
from scipy.signal import minimum_phase

desktop = os.path.join(os.path.expanduser("~"), "Desktop")
pre_path = os.path.join(desktop, "CAB-TR_MPT_PreIR.wav")
post_path = os.path.join(desktop, "CAB-TR_MPT_PostIR.wav")

print("=== CAB-TR MPT Verification ===\n")

# Load PRE and POST IRs
pre_ir, sr = sf.read(pre_path)
post_ir, _ = sf.read(post_path)

# If stereo, use channel 0 for comparison
if pre_ir.ndim > 1:
    pre_ch0 = pre_ir[:, 0]
    post_ch0 = post_ir[:, 0]
else:
    pre_ch0 = pre_ir
    post_ch0 = post_ir

print(f"Sample rate: {sr}")
print(f"IR length: {len(pre_ch0)} samples")
print(f"PRE peak: sample[{np.argmax(np.abs(pre_ch0))}] = {pre_ch0[np.argmax(np.abs(pre_ch0))]:.8f}")
print(f"POST peak (C++): sample[{np.argmax(np.abs(post_ch0))}] = {post_ch0[np.argmax(np.abs(post_ch0))]:.8f}")

# Apply SciPy's minimum_phase (homomorphic method, full length)
scipy_mpt = minimum_phase(pre_ch0, method='homomorphic', half=False)

print(f"SciPy MPT peak: sample[{np.argmax(np.abs(scipy_mpt))}] = {scipy_mpt[np.argmax(np.abs(scipy_mpt))]:.8f}")

# Compare C++ vs SciPy
diff = post_ch0 - scipy_mpt
max_diff = np.max(np.abs(diff))
rms_diff = np.sqrt(np.mean(diff**2))
print(f"\nC++ vs SciPy max |diff|: {max_diff:.2e}")
print(f"C++ vs SciPy RMS diff:   {rms_diff:.2e}")

# Show first 16 samples comparison
print("\nFirst 16 samples comparison:")
print(f"{'idx':>4}  {'PRE':>14}  {'POST(C++)':>14}  {'SciPy':>14}  {'C++-SciPy':>12}")
for i in range(min(16, len(pre_ch0))):
    print(f"{i:4d}  {pre_ch0[i]:14.10f}  {post_ch0[i]:14.10f}  {scipy_mpt[i]:14.10f}  {post_ch0[i]-scipy_mpt[i]:12.2e}")

# Check if PRE is already minimum-phase (idempotency test)
scipy_mpt2 = minimum_phase(scipy_mpt, method='homomorphic', half=False)
idempotency_err = np.max(np.abs(scipy_mpt - scipy_mpt2))
print(f"\nSciPy idempotency check (apply MPT twice): max |diff| = {idempotency_err:.2e}")

# Save SciPy result as WAV for visual comparison in DAW
scipy_path = os.path.join(desktop, "CAB-TR_MPT_SciPy.wav")
sf.write(scipy_path, scipy_mpt, sr, subtype='FLOAT')
print(f"\nSciPy MPT result saved to: {scipy_path}")
print("Load all 3 WAVs in a DAW to visually compare.")
