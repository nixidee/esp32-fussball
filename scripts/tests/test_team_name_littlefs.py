"""Run the actual names writer on the installed LittleFS implementation.

Host flash adapter only; no physical device, filesystem or allocator is used.
The existing corpus fixture supplies values, not an alternative file codec.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
LFS = ROOT / "managed_components/joltwallet__littlefs/src/littlefs"

HARNESS = r'''
extern "C" {
#include "lfs.h"
}
struct Flash {
  std::vector<unsigned char> bytes;
  lfs_t fs{};
  lfs_config config{};
  std::size_t peak = 0, initial = 0, erases = 0;
  explicit Flash(unsigned blocks = 112) : bytes(blocks * 4096, 0xff) {
    config.context = this;
    config.read = [](const lfs_config* c, lfs_block_t b, lfs_off_t o,
                     void* p, lfs_size_t n) {
      const auto& f = *static_cast<Flash*>(c->context);
      std::memcpy(p, f.bytes.data() + b*4096 + o, n); return 0;
    };
    config.prog = [](const lfs_config* c, lfs_block_t b, lfs_off_t o,
                     const void* p, lfs_size_t n) {
      auto& f = *static_cast<Flash*>(c->context);
      for (std::size_t i = 0; i < n; ++i) {
        const auto value = static_cast<const uint8_t*>(p)[i];
        assert((f.bytes[b*4096+o+i] & value) == value);
        f.bytes[b*4096+o+i] = value;
      }
      return 0;
    };
    config.erase = [](const lfs_config* c, lfs_block_t b) {
      auto& f = *static_cast<Flash*>(c->context); ++f.erases;
      std::memset(f.bytes.data()+b*4096, 0xff, 4096); return 0;
    };
    config.sync = [](const lfs_config*) { return 0; };
    config.read_size=128; config.prog_size=128; config.block_size=4096;
    config.block_count=blocks; config.block_cycles=512;
    config.cache_size=512; config.lookahead_size=128;
    assert(lfs_format(&fs, &config) == 0);
    assert(lfs_mount(&fs, &config) == 0);
  }
  ~Flash() { assert(lfs_unmount(&fs)==0); }
  void measure() {
    const auto blocks = lfs_fs_size(&fs); assert(blocks >= 0);
    peak = std::max(peak, std::size_t(blocks)*4096);
  }
  void file(const char* name, std::size_t size) {
    lfs_file_t f{};
    std::array<uint8_t,512> cache{};
    int64_t mtime=1800000000;
    lfs_attr attr{uint8_t('t'),&mtime,sizeof(mtime)};
    lfs_file_config file_config{cache.data(),&attr,1};
    assert(lfs_file_opencfg(&fs,&f,name,LFS_O_CREAT|LFS_O_WRONLY,&file_config)==0);
    std::array<uint8_t,512> data{};
    while (size) {
      const auto n=std::min(size,data.size());
      assert(lfs_file_write(&fs,&f,data.data(),n)==lfs_ssize_t(n)); size-=n;
    }
    assert(lfs_file_close(&fs,&f)==0);
  }
};
struct Output {
  Flash& flash;
  lfs_file_t file{};
  std::array<uint8_t,512> cache{};
  int64_t mtime=1800000000;
  lfs_attr attr{uint8_t('t'),&mtime,sizeof(mtime)};
  lfs_file_config file_config{cache.data(),&attr,1};
  bool opened=false;
  explicit Output(Flash& f) : flash(f) {
    assert(lfs_file_opencfg(&f.fs,&file,cfg::kTeamNamesSlotFiles[1],
                        LFS_O_RDWR|LFS_O_CREAT|LFS_O_EXCL,&file_config)==0);
    opened=true; flash.measure();
  }
  ~Output() { if(opened) assert(lfs_file_close(&flash.fs,&file)==0); }
  static bool read(std::size_t at, void* out, std::size_t n, void* raw) {
    auto& o=*static_cast<Output*>(raw);
    const bool flushing = o.file.flags & LFS_F_WRITING;
    if(lfs_file_seek(&o.flash.fs,&o.file,at,LFS_SEEK_SET)!=lfs_soff_t(at))return false;
    if (flushing) o.flash.measure();
    const bool ok=lfs_file_read(&o.flash.fs,&o.file,out,n)==lfs_ssize_t(n);
    if (flushing) o.flash.measure();
    return ok;
  }
  static bool write(std::size_t at, const void* data, std::size_t n, void* raw) {
    auto& o=*static_cast<Output*>(raw);
    if(lfs_file_seek(&o.flash.fs,&o.file,at,LFS_SEEK_SET)!=lfs_soff_t(at))return false;
    o.flash.measure();
    const bool ok=lfs_file_write(&o.flash.fs,&o.file,data,n)==lfs_ssize_t(n);
    o.flash.measure();return ok;
  }
  st::IO io(){return {this,read,write};}
  void close() {
    assert(lfs_file_sync(&flash.fs,&file)==0);flash.measure();
    assert(lfs_file_close(&flash.fs,&file)==0);opened=false;flash.measure();
  }
};
int main() {
  Fixture fixture;
  std::size_t max_delta=0, max_final=0;
  unsigned cases=0;
  for(unsigned directory : {0,15,31,63,127,240})
    for(unsigned variant : {0,1,2}) {
      Flash flash;
      for(unsigned i=0;i<directory;++i) {
        char name[32];std::snprintf(name,sizeof(name),"entry_%u.bin",i+1);
        flash.file(name,0);
      }
      flash.file(cfg::kTeamNamesSlotFiles[0],16384);
      flash.initial=std::size_t(lfs_fs_size(&flash.fs))*4096;
      flash.peak=flash.initial;
      Output output(flash);
      const auto size=encode(output,fixture,variant!=0,variant==2);assert(size>0);
      if (variant==2) assert(size==cfg::kTeamNamesFileBytes);
      output.close();
      const auto closed_candidate=std::size_t(lfs_fs_size(&flash.fs))*4096;
      lfs_info active{};assert(lfs_stat(&flash.fs,cfg::kTeamNamesSlotFiles[0],&active)==0&&active.size==16384);
      flash.measure();
      const auto delta=flash.peak-flash.initial;
      max_delta=std::max(max_delta,delta);
      max_final=std::max(max_final,closed_candidate-flash.initial);
      std::cout<<"directory="<<directory<<" bytes="<<size
               <<" initial="<<flash.initial<<" peak_delta="<<delta
               <<" closed_candidate_delta="<<closed_candidate-flash.initial<<std::endl;
      ++cases;
    }
  std::cout<<"PASS cases="<<cases<<" max_peak_delta="<<max_delta
           <<" max_closed_candidate_delta="<<max_final<<"\n";
}
'''


def main():
    fixture = (ROOT / "scripts/tests/fixtures/team_name_store_test.cpp").read_text()
    fixture = fixture.split("void recheck(", 1)[0]
    fixture = fixture.replace(
        "std::size_t encode(Memory& m,", "template<class Output>\nstd::size_t encode(Output& m,")
    fixture = fixture.replace("bool maximal = false)",
                              "bool maximal = false, bool boundary = false)")
    fixture = fixture.replace("const st::Fields fields{", """const std::string boundary_league =
        std::string(810, 'x');  // +2-byte pool length = 812 B
    const st::Fields fields{""")
    fixture = fixture.replace("{true, c.league}",
                              "{true, boundary && i == 0 ? std::string_view(boundary_league) : c.league}")
    with tempfile.TemporaryDirectory(prefix="football-names-lfs-") as temporary:
        directory = Path(temporary)
        subprocess.run(["python3", str(ROOT / "scripts/embed_team_names.py"),
                        str(ROOT / "include/team_names.json"), str(directory)], check=True)
        sanitizer = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
        objects = []
        for name in ("lfs.c", "lfs_util.c"):
            output = directory / (name + ".o")
            subprocess.run(["clang", "-std=c11", "-O1", *sanitizer,
                            "-DLFS_NO_DEBUG", "-DLFS_NO_WARN", "-DLFS_NO_ERROR",
                            "-I" + str(LFS), "-c", str(LFS / name), "-o", str(output)], check=True)
            objects.append(str(output))
        harness = directory / "littlefs.cpp"
        harness.write_text(fixture + HARNESS)
        binary = directory / "littlefs"
        subprocess.run(["clang++", "-std=c++20", "-O1", *sanitizer,
                        "-I" + str(ROOT / "include"),
                        "-I" + str(ROOT / "components/names/include"),
                        "-I" + str(directory / "team_name_asset"), "-I" + str(LFS),
                        str(harness), str(ROOT / "components/names/team_name_store.cpp"),
                        str(ROOT / "components/names/team_name_catalogue.cpp"),
            str(ROOT / "components/json/json_stream.cpp"),
            "-I" + str(ROOT / "components/json/include"),
                        *objects, "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
