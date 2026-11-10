// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "files_volume_archive.hpp"
#include "native_filesystem.hpp"
#include "private_journal_scope.hpp"
#include "sha256.hpp"
#include "archive_fault_allocator.hpp"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <new>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
using namespace ck::files;using namespace ck::vision;
namespace {
using archive_test::deny_alloc;
const char* mode="roundtrip";bool armed=false,known=false,write_seen=false,close_fault=false,flush_fault=false,linked=false;
dev_t stage_dev=0;ino_t stage_ino=0;int closed=0,reused=-1;std::stop_source* cancel_source=nullptr;
std::string base_path;bool stage(int fd){struct stat s{};return known&&fstat(fd,&s)==0&&s.st_dev==stage_dev&&s.st_ino==stage_ino;}
bool is(const char* text){return !std::strcmp(mode,text);}
}
extern "C" int __real_ftruncate(int,off_t);
extern "C" int __wrap_ftruncate(int fd,off_t n){if(armed){struct stat s{};assert(fstat(fd,&s)==0&&S_ISREG(s.st_mode)&&s.st_nlink==0);known=true;stage_dev=s.st_dev;stage_ino=s.st_ino;}
 return __real_ftruncate(fd,n);}
extern "C" ssize_t __real_pwrite(int,const void*,size_t,off_t);
extern "C" ssize_t __wrap_pwrite(int fd,const void* data,size_t n,off_t at){
 const auto r=__real_pwrite(fd,data,n,at);
 if(armed&&stage(fd)&&!write_seen){write_seen=true;
  if(is("hint-replaced")){const auto hint=base_path+"/dest/.image.ckarchive.cktmp-1";const auto saved=base_path+"/dest/saved";assert(rename(hint.c_str(),saved.c_str())==0);int x=open(hint.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);assert(x>=0&&write(x,"replacement",11)==11&&close(x)==0);}
  if(is("cancel-in-write")||is("cancel-close")||is("cancel-close-oom")){assert(cancel_source);cancel_source->request_stop();}
  if(is("upstream-oom-close"))deny_alloc=true;
 }
 return r;
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd){
 if(armed&&stage(fd)&&is("first-enospc-close-oom")&&!flush_fault){flush_fault=true;errno=ENOSPC;return -1;}
 if(armed&&stage(fd)&&linked&&is("post-link-flush")&&!flush_fault){flush_fault=true;errno=EIO;return -1;}
 return __real_fsync(fd);
}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd){const bool target=armed&&stage(fd);if(target)++closed;const int r=__real_close(fd);
 if(target&&!close_fault&&(is("close-oom")||is("first-enospc-close-oom")||is("upstream-oom-close")||is("cancel-close")||is("cancel-close-oom"))){
  assert(r==0);close_fault=true;reused=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(reused==fd);
  if(!is("cancel-close"))deny_alloc=true;
  errno=EIO;return -1;}
 return r;}
extern "C" int __real_linkat(int,const char*,int,const char*,int);
extern "C" int __wrap_linkat(int a,const char* b,int c,const char* d,int e){const int r=__real_linkat(a,b,c,d,e);if(armed&&r==0){linked=true;if(is("cancel-after-publish")){assert(cancel_source);cancel_source->request_stop();}}return r;}
namespace {
template<class T>T take(Observation<T> r){if(!r.value()){std::fprintf(stderr,"unavailable %d/%lld %s\n",int(r.state()),(long long)r.unavailable_detail()->error.code,r.unavailable_detail()->error.operation.c_str());std::abort();}return std::move(r).take_value();}
NativeName name(const char* s){return *NativeName::from_posix(s);}NativeAbsolutePath address(const std::string& s){return take(NativeAbsolutePath::from_posix(s));}
OperationJournalId id(){OperationJournalId x{};x.bytes[0]=std::byte{1};return x;}
std::vector<std::byte> bytes(const char* s){const auto* p=reinterpret_cast<const std::byte*>(s);return {p,p+strlen(s)};}
FilesVolumeRecord source(){return {"Fixture volume","/fixture","ext4",64,32,false,"archive-native-fixture",{FilesVolumeArchiveSupport::LiveReadable,false,{}},{}};}
FilesVolumeArchiveDocument document(){FilesVolumeArchiveDocument d;d.source=source();d.entries={{".",FilesVolumeArchiveEntryKind::Directory,0,{},{},0},{"sparse.bin",FilesVolumeArchiveEntryKind::RegularFile,2*1024*1024,{{0,bytes("DATA")},{2*1024*1024-4,bytes("TAIL")}}, {},0}};return d;}
std::string journal(){return ".ck-volume-archive-01000000000000000000000000000000";}
std::vector<std::byte> read_file(const std::string& path){int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC);assert(fd>=0);struct stat s{};assert(fstat(fd,&s)==0);std::vector<std::byte> data(static_cast<std::size_t>(s.st_size));assert(pread(fd,data.data(),data.size(),0)==static_cast<ssize_t>(data.size()));assert(close(fd)==0);return data;}
std::uint64_t little(const std::vector<std::byte>& b,std::size_t at,unsigned n){assert(at+n<=b.size());std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(std::to_integer<unsigned char>(b[at+i]))<<(8*i);return v;}
void verify_archive(const std::string& path,OperationJournalState expected_state=OperationJournalState::Complete){auto b=read_file(path);assert(b.size()>56&&!memcmp(b.data(),"CKARV01\0",8)&&little(b,8,4)==2);const auto data=little(b,24,8);assert(data==56+little(b,16,8)&&little(b,32,8)==2*1024*1024&&little(b,40,8)==2&&b.size()==data+2*1024*1024);
 assert(!memcmp(b.data()+data,"DATA",4)&&!memcmp(b.data()+b.size()-4,"TAIL",4));for(std::size_t i=data+4;i+4<b.size();++i)assert(b[i]==std::byte{});
 struct stat s{};assert(stat(path.c_str(),&s)==0&&s.st_blocks*512<s.st_size/4);
 auto journal_bytes=read_file(base_path+"/private/"+journal());auto decoded=take(decode_operation_journal(journal_bytes));assert(!decoded.tail);
 auto recovery=take(inspect_volume_archive_recovery(*decoded.journal));assert(recovery.evidence.staging_namespace==StagingNamespace::Anonymous&&!recovery.evidence.staging_component&&!recovery.evidence.staging_encoding&&recovery.evidence.staging_native_path.empty());
 Sha256 hash;hash.update(b);assert(recovery.evidence.staging_digest&&*recovery.evidence.staging_digest==hash.digest()&&recovery.evidence.archive_logical_size==b.size());
 assert(decoded.journal->state(0)==expected_state);
}
}
int main(int argc,char** argv){assert(argc==3&&getuid()!=0);mode=argv[2];base_path=argv[1];assert(mkdir((base_path+"/dest").c_str(),0700)==0&&mkdir((base_path+"/private").c_str(),0700)==0);
 // Caller durably provisions the private Root namespace before acquisition.
 int parent=open(base_path.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(parent>=0&&fsync(parent)==0&&close(parent)==0);
 NativeFileSystem fs;auto dest=std::shared_ptr<const DirectoryCursor>(take(fs.open_native_directory(address(base_path+"/dest"))));auto root=take(acquire_native_journal_root(address(base_path+"/private")));
 if(is("no-owner")){FilesVolumeArchiveWriteRequest request{document(),base_path+"/nonexistent","image.ckarchive",1,{},{},{},{},{},{},{}};unsigned callbacks=0;request.append_journal=[&](auto){++callbacks;return Observation<bool>::available(true);};auto result=write_volume_archive(request);assert(!result.value()&&result.state()==ObservationState::Unsupported&&callbacks==0);dest.reset();take(root->finish_io());return 0;}
 if(is("occupied")){int fd=open((base_path+"/dest/image.ckarchive").c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0&&write(fd,"original",8)==8&&close(fd)==0);}
 if(is("hint-replaced")){int fd=open((base_path+"/dest/.image.ckarchive.cktmp-1").c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);assert(fd>=0&&write(fd,"original",8)==8&&close(fd)==0);}
 std::stop_source stop;cancel_source=&stop;armed=true;
 const bool service_mode=is("service")||is("service-legacy-root")||is("service-oom");
 if(service_mode){if(is("service-oom"))mode="upstream-oom-close";
  FilesVolumeArchiveService worker;FilesVolumeArchiveRequest request{source(),address(base_path+"/dest"),"image.ckarchive",FilesVolumeArchiveConsistency::Live,1,[](auto){return Observation<FilesVolumeArchiveDocument>::available(document());},address(base_path+"/private"),id(),{},[](const auto&,const auto&,bool)->Observation<std::unique_ptr<OperationJournalFile>>{std::abort();},{}};
  if(!is("service-legacy-root"))request.prepared_journal_root=root;
  auto submitted=worker.request(std::move(request));assert(submitted.state==FilesVolumeArchiveSubmission::Accepted);
  std::optional<FilesVolumeArchiveReply> reply;for(unsigned i=0;i<30000&&!reply;++i){reply=worker.take();if(!reply)std::this_thread::sleep_for(std::chrono::milliseconds(1));}assert(reply);worker.request_stop();worker.join();
  if(is("upstream-oom-close")){assert(reply->state==FilesVolumeArchiveState::WorkerFault&&reply->failure_pod&&reply->failure_pod->domain==ErrorDomain::Posix&&reply->failure_pod->code==EIO&&close_fault);}
  else {assert(reply->state==FilesVolumeArchiveState::Complete&&reply->result&&reply->result->publication_confirmed);}
 }else{
  FilesVolumeArchiveWriteRequest request{document(),base_path+"/dest","image.ckarchive",1,{},{},{},{},{},{},{}};request.destination_owner=dest;request.prepared_journal_root=root;request.native_journal_name=name(journal().c_str());request.native_journal_id=id();
  try{auto result=write_volume_archive(request,stop.get_token());
   if(is("occupied"))assert(!result.value()&&result.state()==ObservationState::Changed);
   else if(is("cancel-in-write"))assert(!result.value()&&result.unavailable_detail()->error.code==static_cast<std::int64_t>(AdapterError::InterruptedRead));
   else if(is("cancel-close"))assert(!result.value()&&result.unavailable_detail()->error.domain==ErrorDomain::Posix&&result.unavailable_detail()->error.code==EIO);
   else if(is("first-enospc-close-oom"))assert(!result.value()&&result.unavailable_detail()->error.domain==ErrorDomain::Posix&&result.unavailable_detail()->error.code==ENOSPC);
   else if(is("cancel-close-oom")||is("upstream-oom-close"))assert(!result.value()&&result.unavailable_detail()->error.domain==ErrorDomain::Posix&&result.unavailable_detail()->error.code==EIO);
   else if(is("close-oom"))assert(result.value()&&result.value()->publication==FilesVolumeArchivePublication::PublishedDurabilityUnknown&&result.value()->publication_confirmed);
   else if(is("post-link-flush"))assert(result.value()&&result.value()->publication==FilesVolumeArchivePublication::PublishedDurabilityUnknown&&!result.value()->publication_confirmed&&result.value()->digests_verified);
   else {assert(result.value()&&result.value()->publication==FilesVolumeArchivePublication::Durable&&result.value()->publication_confirmed);}
  }catch(const std::bad_alloc&){assert(is("close-oom")||is("first-enospc-close-oom")||is("upstream-oom-close")||is("cancel-close-oom"));}
 }
 // This ENOSPC path can format entirely in small-string storage. A separate
 // allocator control proves denial remains active without demanding that the
 // production drain allocate merely to exercise the fault switch.
 if(is("first-enospc-close-oom")&&archive_test::denied_allocations.load()==0){bool rejected=false;
  try{void* allocation=::operator new(1);::operator delete(allocation);}catch(const std::bad_alloc&){rejected=true;}assert(rejected);}
 deny_alloc=false;armed=false;assert(closed==1);if(is("close-oom")||is("first-enospc-close-oom")||is("upstream-oom-close")||is("cancel-close-oom"))assert(archive_test::denied_allocations.load()>0);if(reused>=0){assert(fcntl(reused,F_GETFD)>=0);__real_close(reused);}
 if(is("close-oom")||is("first-enospc-close-oom")||is("upstream-oom-close")||is("cancel-close")||is("cancel-close-oom")){
  detail::PreparedJournalConsumerFailure failure;assert(detail::read_prepared_journal_consumer_failure(*root,failure)&&!failure.pending&&failure.domain==ErrorDomain::Posix&&failure.code==(is("first-enospc-close-oom")?ENOSPC:EIO));}
 if(is("roundtrip")||is("service")||is("service-legacy-root")||is("cancel-after-publish")||is("hint-replaced"))verify_archive(base_path+"/dest/image.ckarchive");
 if(is("post-link-flush"))verify_archive(base_path+"/dest/image.ckarchive",OperationJournalState::CommitIntended);
 if(is("occupied"))assert(read_file(base_path+"/dest/image.ckarchive")==bytes("original"));
 if(is("hint-replaced")){assert(read_file(base_path+"/dest/saved")==bytes("original")&&read_file(base_path+"/dest/.image.ckarchive.cktmp-1")==bytes("replacement"));}
 else {struct stat s{};assert(lstat((base_path+"/dest/.image.ckarchive.cktmp-1").c_str(),&s)!=0&&errno==ENOENT);}
 dest.reset();auto finished=root->finish_io();if(is("roundtrip")||is("service")||is("service-legacy-root")||is("cancel-after-publish")||is("hint-replaced")||is("cancel-in-write"))assert(finished.value());
 std::printf("adapter mode=%s PASS original-creator-close=%d no-reopen/no-path-cleanup\n",mode,closed);
}
