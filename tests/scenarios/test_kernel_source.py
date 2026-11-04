"""Real Git fixture: a reversible patch can coexist with an unrelated modification."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0,str(Path(__file__).resolve().parents[2]/"tools"))
import kernel_source as ks


class ExactSource(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.root=Path(self.tmp.name);self.work=self.root/"build/linux-src"
        self.work.mkdir(parents=True)
        self.git("init","-q")
        self.git("config","user.name","Fixture");self.git("config","user.email","fixture@example.invalid")
        self.files=["drivers/pci/iov.c","drivers/iommu/intel/iommu.c"]
        for name in self.files:
            p=self.work/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text("base\n"+"keep\n"*12)
        self.git("add",".");self.git("commit","-qm","base")
        commit=self.git("rev-parse","HEAD").decode().strip()
        for name in self.files:
            p=self.work/name;p.write_text(p.read_text().replace("base\n","patched\n"))
        change=self.root/"locked.patch";change.write_bytes(self.git("diff","HEAD"))
        (self.root/"tools").mkdir();(self.root/"tools/kernel_source.py").write_bytes(Path(ks.__file__).read_bytes())
        self.lock={"sources":{"linux":{"commit":commit,"patches":[{"path":"locked.patch","sha256":ks.sha256(change)}]}}}
    def tearDown(self):self.tmp.cleanup()
    def git(self,*args):return subprocess.check_output(["git",*args],cwd=self.work,stderr=subprocess.PIPE)
    def check(self):
        with patch.object(ks,"ROOT",self.root):return ks.source_inputs(self.lock)
    def test_exact_patch_is_accepted_without_index_change(self):
        before=(self.work/".git/index").read_bytes()
        self.assertEqual(set(self.check()["patched_files"]),set(self.files))
        self.assertEqual((self.work/".git/index").read_bytes(),before)
    def test_extra_change_in_allowed_file_rejected(self):
        p=self.work/self.files[0];p.write_text(p.read_text()+"unlocked modification\n")
        self.git("apply","--reverse","--check",str(self.root/"locked.patch"))
        with self.assertRaisesRegex(RuntimeError,"differs from exact"):self.check()
    def test_changed_head_rejected(self):
        self.git("add",".");self.git("commit","-qm","different head")
        with self.assertRaisesRegex(RuntimeError,"source commit differs"):self.check()


if __name__=="__main__":unittest.main()
