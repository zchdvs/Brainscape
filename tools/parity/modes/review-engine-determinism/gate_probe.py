# Probe: does tools/ci/sound_rev_gate.py notice a corpus package whose sound_hash changed
# while its rendered audio hash did not (design §8.3, §10.3 "golden.json records each
# package's sound_hash")?
import importlib.util, os, sys
spec = importlib.util.spec_from_file_location(
    "gate", os.path.join(os.path.dirname(os.path.abspath(__file__)), "../../../ci/sound_rev_gate.py"))
gate = importlib.util.module_from_spec(spec); spec.loader.exec_module(gate)
base = {"soundRevision": 2, "vectors": [{"name": "plucks_12s", "presets": [
    {"name": "factory.engram", "hash": "aa"*32, "secondHashes": ["11"], "soundHash": "b0"*32}]}]}
head = {"soundRevision": 2, "vectors": [{"name": "plucks_12s", "presets": [
    {"name": "factory.engram", "hash": "aa"*32, "secondHashes": ["11"], "soundHash": "c1"*32}]}]}
print("golden_changes with only soundHash changed:", gate.golden_changes(base, head))
# Added when the probe was kept (mode-compiler lane G): the package rule now reads soundHash.
if hasattr(gate, "package_changes"):
    print("package_changes with only soundHash changed:", gate.package_changes(base, head, {}, {}))
