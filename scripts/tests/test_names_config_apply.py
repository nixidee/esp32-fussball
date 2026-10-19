#!/usr/bin/env python3
"""Actual names files + application owner: coherent reboot and uncertain NVS.

Controlled SDK/network/mount adapters; actual POSIX files, codecs, settings and
names application code. No personal preset expansion and no device claim.
"""
from pathlib import Path
import ast
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
# Reuse the same controlled NVS adapter rather than another persistence model.
module = ast.parse((ROOT / "scripts/tests/test_app_config_owner.py").read_text())
stubs = next(ast.literal_eval(node.value) for node in module.body
             if isinstance(node, ast.Assign) and any(isinstance(target, ast.Name)
             and target.id == "stubs" for target in node.targets))
stubs = stubs.replace('std::map<std::string,std::vector<uint8_t>> durable;',
                      'inline std::map<std::string,std::vector<uint8_t>> durable;')
stubs = stubs.replace('unsigned fault=0,writes=0; bool mutated=false,fail_open=false;',
                      'inline unsigned fault=0,writes=0; inline bool mutated=false,fail_open=false;')
stubs += '\ninline constexpr int ESP_ERR_NOT_FINISHED=12;\n'
source = (ROOT / "components/settings/settings_store.cpp").read_text()
source = source[source.index("namespace settings {"):]
a = source.index("Presets presets()"); b = source.index("\n}\n", a) + 2
source = source[:a] + 'Presets presets() { return {}; }' + source[b:]
main = r'''
#include "team_name_service.h"
#include "application_config.h"
#include "team_name_json.h"
#include "file_service.h"
#include <filesystem>
#include <fstream>
namespace settings {
void rebootOwner() { generation_counter=0;data_generation_counter=0;
 record_unread=false;current_source=Origin::kInitial;init(); }
}
void boot() {fault=0;mutated=false;settings::rebootOwner();football::names::invalidate();}
namespace names=football::names;
void stage(names::files::Candidate& candidate,const char* de) {
 assert(names::begin(candidate)==ESP_OK);
 names::store::Counts counts;counts.clubs=1;counts.aliases=1;
 counts.bindings[0]=1;counts.exceptions=1;
 names::store::Writer out;
 const auto next_generation=settings::namesReference().generation+1;
 assert(out.begin(candidate.io(),counts,next_generation,123));
 assert(out.key("fc-bayern"));assert(out.binding(cfg::Provider::kOpenLigaDb,40,0));
 names::store::Fields fields{{true,de},{true,"Bayern Munich"},{true,"FCB"},{true,"Bundesliga"}};
 assert(out.club(0,fields));std::string alias(1024,'a');alias+="😀";
 assert(out.alias(0,alias));assert(out.exception(cfg::Provider::kEspn,999,""));
 const auto size=out.finish();assert(size>0&&candidate.validate(size)==names::files::Status::kOk);
}
std::string visible(uint8_t language) {
 names::Lookup lookup;assert(lookup.open()==ESP_OK);
 names::Effective result;
 assert(names::resolve(lookup.reader(),cfg::Provider::kOpenLigaDb,40,"Provider Original","PRO",language,result));
 assert(lookup.close());return result.name.data;
}
struct Sink { std::string bytes;
 static bool write(std::string_view text,void* raw){static_cast<Sink*>(raw)->bytes.append(text);return true;}
};
esp_err_t applyNames(settings::AppConfig& config, uint32_t revision,
                     names::files::Candidate& candidate) {
 settings::NamesChange change;
 const auto prepared=names::prepareChange(candidate,change);
 return prepared==ESP_OK?settings::apply(config,revision,nullptr,nullptr,&change):prepared;
}
void namesTest() {
 settings::AppConfig config;
 durable.clear();boot();settings::snapshot(config);
 assert(config.names==settings::NamesRef{});
 { names::files::Candidate candidate;stage(candidate,"FC Bayern München");
   config.model.hostname.assign("first");config.last_apply_id=UINT64_C(0xfedcba9876543210);
   assert(applyNames(config,config.revision,candidate)==ESP_OK);
 }
 assert(writes==1&&durable.size()==1&&settings::namesReference().slot==1);
 assert(visible(0)=="FC Bayern München"&&visible(1)=="Bayern Munich");
 const auto first_reference=settings::namesReference();
 files::free_bytes=cfg::kFileReplacementWorkspaceBytes-1;
 {names::files::Candidate candidate;assert(names::begin(candidate)==files::kStorageFull);}
 files::free_bytes=458752;
 assert(settings::namesReference()==first_reference&&writes==1&&visible(0)=="FC Bayern München");
 names::VisibleCache cache;names::Targets targets;char label[41]="Provider Original",short_name[4]="PRO";
 char exception_label[41]="Unchanged Full",exception_short[4]="ZZZ";
 char fallback_label[41]="Fresh Provider",fallback_short[4]="NEW";
 targets.add({40,cfg::Provider::kOpenLigaDb,label,sizeof(label),short_name,sizeof(short_name)});
 targets.add({999,cfg::Provider::kEspn,exception_label,sizeof(exception_label),exception_short,sizeof(exception_short)});
 targets.add({12345,cfg::Provider::kEspn,fallback_label,sizeof(fallback_label),fallback_short,sizeof(fallback_short)});
 assert(names::resolveVisible(targets,0,settings::generation(),cache)==ESP_OK&&std::string(label)=="FC Bayern München"&&std::string(short_name)=="FCB");
 assert(std::string(exception_label)=="Unchanged Full"&&exception_short[0]==0);
 operations::available=false;std::strcpy(label,"Renamed Provider");std::strcpy(short_name,"NEW");
 std::strcpy(exception_short,"ZZZ");std::strcpy(fallback_label,"New Fallback");std::strcpy(fallback_short,"NOW");
 assert(names::resolveVisible(targets,0,settings::generation(),cache)==ESP_OK&&std::string(label)=="FC Bayern München"&&std::string(short_name)=="FCB");
 assert(std::string(exception_label)=="Unchanged Full"&&exception_short[0]==0);
 assert(std::string(fallback_label)=="New Fallback"&&std::string(fallback_short)=="NOW");
 operations::available=true;
 // A close failure must not publish a complete cache that hides the error.
 names::VisibleCache failed_cache;
 close_failure=true;
 assert(names::resolveVisible(targets,0,settings::generation(),failed_cache)==ESP_FAIL);
 assert(!close_failure && failed_cache.count==0);
 operations::available=false;
 assert(names::resolveVisible(targets,0,settings::generation(),failed_cache)==ESP_ERR_NOT_FINISHED);
 operations::available=true;
 assert(names::resolveVisible(targets,0,settings::generation(),failed_cache)==ESP_OK);
 assert(failed_cache.count==targets.count);
 // No names rewrite or filesystem dependency for a settings-only commit.
 const auto ref=settings::namesReference();files::mounted=false;
 settings::snapshot(config);config.model.hostname.assign("settings-only");
 assert(settings::apply(config,config.revision)==ESP_OK&&settings::namesReference()==ref);
 files::mounted=true;
 // The same names reference is insufficient: a stale language/config snapshot
 // must neither reuse cached adopted labels nor alter the caller's fallback.
 const auto old_revision=config.revision-1;
 const auto cache_count=cache.count;const auto cache_ref=cache.reference;
 std::strcpy(label,"Untouched stale view");std::strcpy(short_name,"OLD");
 operations::available=false;
 assert(names::resolveVisible(targets,1,old_revision,cache)==ESP_ERR_INVALID_STATE);
 assert(std::string(label)=="Untouched stale view"&&std::string(short_name)=="OLD");
 assert(cache.count==cache_count&&cache.reference==cache_ref&&cache.language==0);
 names::VisibleCache stale_empty;
 assert(names::resolveVisible(targets,0,old_revision,stale_empty)==ESP_ERR_INVALID_STATE);
 assert(stale_empty.count==0&&stale_empty.reference==settings::NamesRef{});
 operations::available=true;
 assert(names::resolveVisible(targets,1,config.revision,cache)==ESP_OK);
 assert(std::string(label)=="Bayern Munich"&&cache.language==1);
 Sink sink;json::Writer json({&sink,Sink::write});
 names::AdoptedExport exported;
 assert(exported.prepare()==ESP_OK&&exported.write(json)==ESP_OK&&json.finish());
 assert(sink.bytes.find("fc-bayern")!=std::string::npos&&sink.bytes.find("\"version\":2")!=std::string::npos);
 assert(sink.bytes.find("\"texts\":{")!=std::string::npos&&sink.bytes.find("😀")!=std::string::npos);
 struct Input {std::string_view text;std::size_t at=0;
   static int read(void* raw){auto& in=*static_cast<Input*>(raw);return in.at<in.text.size()?static_cast<uint8_t>(in.text[in.at++]):-1;}};
 {names::files::Candidate candidate;assert(names::begin(candidate)==ESP_OK);
  Input input{sink.bytes};json::Reader reader({&input,Input::read});json::Token object;settings::Violation invalid;
  assert(reader.next(object)&&names::readAdoptedJson(reader,object,candidate,invalid)==names::Decode::kOk&&reader.finish());
  names::files::Read checked;assert(checked.open(candidate)==names::files::Status::kOk);
  names::Effective labels;assert(names::resolve(&checked.reader(),cfg::Provider::kOpenLigaDb,40,"Fallback","PRO",0,labels));
  assert(std::string(labels.name.data)=="FC Bayern München"&&std::string(labels.short_name.data)=="FCB");
  assert(names::resolve(&checked.reader(),cfg::Provider::kEspn,999,"Retained Full","PRO",0,labels));
  assert(std::string(labels.name.data)=="Retained Full"&&labels.short_name.data[0]==0);
  assert(checked.close());}
 // Complete inactive candidate, crash before config commit: old reference wins.
 settings::snapshot(config);const auto before=config.names;
 { names::files::Candidate candidate;stage(candidate,"Not yet adopted");candidate.preserve(); }
 boot();assert(settings::namesReference()==before&&visible(0)=="FC Bayern München");
 for(unsigned failure=1;failure<=5;++failure) {
   boot();settings::snapshot(config);const auto old=config;
   { names::files::Candidate candidate;stage(candidate,("New " + std::to_string(failure)).c_str());
     config.model.hostname.assign("fail-" + std::to_string(failure));config.last_apply_id++;
     fault=failure;mutated=false;
     assert(applyNames(config,old.revision,candidate)!=ESP_OK);
     settings::AppConfig observed;settings::snapshot(observed);
     assert(observed.names==old.names&&observed.model==old.model&&observed.revision==old.revision);
     assert(settings::origin()==settings::Origin::kOutcomeUnknown);
     names::files::Candidate blocked;assert(names::begin(blocked)==ESP_ERR_INVALID_STATE);
   }
   for(const char* file:cfg::kTeamNamesSlotFiles)
     assert(std::filesystem::is_regular_file(std::filesystem::path(cfg::kFsBasePath)/file));
   boot();settings::snapshot(config);
   if(failure==1)assert(config.names==old.names&&config.model==old.model);
   else assert(config.model.hostname.view()=="fail-"+std::to_string(failure)&&visible(0)=="New "+std::to_string(failure));
 }
 // Simulate interruption after durable NVS but before names RAM activation.
 boot();settings::snapshot(config);const auto cut_revision=config.revision;
 { names::files::Candidate candidate;stage(candidate,"After reboot");
   settings::NamesChange change;assert(names::prepareChange(candidate,change)==ESP_OK);
   change.activate=[](void*){return ESP_FAIL;};
   assert(settings::apply(config,cut_revision,nullptr,nullptr,&change)==ESP_FAIL);
   assert(settings::origin()==settings::Origin::kOutcomeUnknown);
   assert(settings::generation()==cut_revision&&settings::namesReference()==candidate.expectedReference());
 }
 boot();assert(visible(0)=="After reboot");
 // A missing referenced file fails; no false empty-success fallback.
 const auto active=settings::namesReference();
 std::filesystem::remove(std::filesystem::path(cfg::kFsBasePath)/cfg::kTeamNamesSlotFiles[active.slot-1]);
 names::invalidate();assert(names::readStatus()!=ESP_OK);
 files::mounted=false;settings::snapshot(config);config.model.hostname.assign("fs-offline");
 assert(settings::apply(config,config.revision)==ESP_OK);
 std::cout<<"names/config: exact refs, two slots, reboot cuts,5 uncertain NVS results, visible cache, streamed aliases, settings without filesystem PASS\n";
}
void applicationSessionTest() {
 files::mounted=true;operations::available=true;durable.clear();boot();
 std::string adopted;
 { application::ConfigExportSession output;
   assert(output.admitted()&&output.prepare()==ESP_OK);
   Sink sink;json::Writer writer({&sink,Sink::write});
   assert(output.writeNames(writer)==ESP_OK&&writer.finish());adopted=sink.bytes;
 }
 struct Input {std::string_view bytes;size_t at=0;static int read(void* raw){auto& in=*static_cast<Input*>(raw);return in.at<in.bytes.size()?static_cast<unsigned char>(in.bytes[in.at++]):-1;}};
 settings::Violation why;
 { application::ApplySession session;assert(session.admitted()&&session.prepare()==ESP_OK);
   auto& draft=session.draft();const auto revision=draft.revision;
   draft.model.hostname.assign("session-apply");++draft.last_apply_id;
   Input input{adopted};json::Reader reader({&input,Input::read});json::Token token;assert(reader.next(token));
   assert(session.stageNames(reader,token,why)==names::Decode::kOk&&reader.finish());
   const auto before_writes=writes;
   const auto result=session.publish(revision,true,why);
   assert(result.error==ESP_OK&&result.stage==application::ApplyStage::kNone&&writes==before_writes+1);
 }
 boot();settings::AppConfig current;settings::snapshot(current);
 assert(current.model.hostname.view()=="session-apply"&&current.names.slot);
 const auto old_reference=current.names;
 { application::ApplySession session;assert(session.prepare()==ESP_OK);
   const auto result=session.publish(session.draft().revision+1,false,why);
   assert(result.stage==application::ApplyStage::kRevision&&settings::namesReference()==old_reference);
 }
 { application::ApplySession session;assert(session.prepare()==ESP_OK);
   session.draft().model.hostname.assign("session-uncertain");++session.draft().last_apply_id;
   fault=1;mutated=false;const auto result=session.publish(session.draft().revision,false,why);
   assert(result.error!=ESP_OK&&result.stage==application::ApplyStage::kUncertain);
   assert(settings::namesReference()==old_reference);
 }
 boot();assert(settings::namesReference()==old_reference);
 operations::available=false;
 {application::ApplySession blocked;assert(!blocked.admitted()&&blocked.prepare()==ESP_ERR_NOT_FINISHED);}
 {application::ConfigExportSession blocked;assert(!blocked.admitted()&&blocked.prepare()==ESP_ERR_NOT_FINISHED);}
 operations::available=true;
 std::cout<<"actual application sessions: coherent names+config one write, checked export/roundtrip, stale rejection, uncertain outcome/reference preservation, admission PASS\n";
}
int main(){std::filesystem::create_directories(cfg::kFsBasePath);namesTest();applicationSessionTest();}
'''
# Exercise the actual HTTP bridge as well as the application session. Inject
# source, not another implementation of Apply or its names/NVS transaction.
from test_team_names_http import block
http_source=(ROOT/'components/web/web_service.cpp').read_text()
http_fixture=(ROOT/'scripts/tests/fixtures/application_apply_http.h').read_text()
http_fixture=http_fixture.replace('// ACTUAL_HTTP_APPLY',block(http_source,'esp_err_t applyConfig('))
main=main.replace('int main(){std::filesystem::create_directories(cfg::kFsBasePath);namesTest();applicationSessionTest();}',
    http_fixture+'\nint main(){std::filesystem::create_directories(cfg::kFsBasePath);namesTest();applicationSessionTest();http_test::test();}')
with tempfile.TemporaryDirectory(prefix="names-config-", dir="/tmp") as temp:
    folder = Path(temp)
    fs = folder / "files"
    config = (ROOT / "include/app_config.h").read_text().replace('kFsBasePath[] = "/fs"',
                 'kFsBasePath[] = "' + str(fs) + '"')
    (folder / "app_config.h").write_text(config)
    (folder / "esp_err.h").write_text(stubs)
    (folder / "operation_admission.h").write_text('''#pragma once
namespace operations {inline bool available=true;struct Guard {explicit operator bool() const{return available;}};}
''')
    (folder / "ota_service.h").write_text('#pragma once\nnamespace ota {inline bool trial(){return false;}}\n')
    (folder / "file_service.h").write_text('#pragma once\n#include "' +
        str(ROOT / 'components/files/include/file_service.h') + '"\n' + '''
namespace files {inline bool mounted=true;inline std::size_t free_bytes=458752;
inline State state(){return mounted?State::kMounted:State::kUnavailable;}
inline int usage(Usage& out){out={458752,458752-free_bytes};return mounted?ESP_OK:ESP_FAIL;}}
''')
    (folder / "main.cpp").write_text('#include "settings_store.h"\n#include "app_config_record.h"\n#include "operation_admission.h"\n' + source + main)
    includes = [folder, ROOT / "include", ROOT / "components/web"] + list((ROOT / "components").glob("*/include"))
    # Control only the POSIX close syscall; keep the actual checked I/O owner.
    io = (ROOT / "components/files/file_io.cpp").read_text()
    (folder / "file_io.cpp").write_text(
        '#include <unistd.h>\ninline bool close_failure=false;\n'
        'int controlledClose(int fd){const int result=::close(fd);'
        'if(close_failure){close_failure=false;return -1;}return result;}\n'
        + io.replace('::close(', 'controlledClose('))
    (folder / "main.cpp").write_text(
        '#include "settings_store.h"\n#include "app_config_record.h"\n'
        '#include "operation_admission.h"\nextern bool close_failure;\n' + source + main)
    sources = [folder / "main.cpp", folder / "file_io.cpp"]
    for component, files in {
      'application': ('config.cpp',),
      'web': ('config_json.cpp','settings_json.cpp','screen_json.cpp'),
      'settings': ('app_config_model.cpp','app_config_record.cpp','settings_model.cpp','settings_record.cpp','screen_policy.cpp','screen_record.cpp','ui_config.cpp','runtime_config.cpp'),
      'timekeeping': ('civil_time.cpp',), 'json': ('json_stream.cpp',),
      'files': ('file_names.cpp','file_candidate.cpp'),
      'football': ('football_model.cpp',),
      'names': ('team_name_service.cpp','team_name_files.cpp','team_name_store.cpp','effective_names.cpp','team_name_json.cpp')
    }.items(): sources += [ROOT / 'components' / component / name for name in files]
    binary = folder / "check"
    command = ['clang++','-std=c++20','-g','-O1','-Wall','-Wextra','-Werror',
               '-fsanitize=address,undefined','-fno-omit-frame-pointer']
    command += ['-I' + str(path) for path in includes] + [str(path) for path in sources] + ['-o',str(binary)]
    subprocess.run(command,check=True)
    subprocess.run([str(binary)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0:abort_on_error=1','UBSAN_OPTIONS':'halt_on_error=1'})
