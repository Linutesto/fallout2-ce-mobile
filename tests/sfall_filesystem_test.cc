// sfall_filesystem: script edits must reach engine reads and saved handles survive reload.
#include "sfall_filesystem.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
namespace fallout {
void artCacheFlush() {}
int dbGetFileSize(const char*, int* n) { *n = 8; return 0; }
int dbGetFileContents(const char*, void* out) { unsigned char b[] = {0,1,2,3,4,5,6,7}; memcpy(out,b,8); return 0; }
size_t fileRead(void* b,size_t s,size_t n,File* f) { return fread(b,s,n,f->file); }
size_t fileWrite(const void* b,size_t s,size_t n,File* f) { return fwrite(b,s,n,f->file); }
int fileWriteInt32(File* f,int32_t v) {
    unsigned char b[] = {(unsigned char)(v>>24),(unsigned char)(v>>16),(unsigned char)(v>>8),(unsigned char)v};
    return fwrite(b,1,4,f->file)==4?0:-1;
}
int fileReadInt32(File* f,int32_t* v) {
    unsigned char b[4]; if(fread(b,1,4,f->file)!=4)return -1;
    *v=(uint32_t(b[0])<<24)|(uint32_t(b[1])<<16)|(uint32_t(b[2])<<8)|b[3];return 0;
}
}
using namespace fallout;
int main() {
    int copied=sfallFileSystemCopy("art/test.frm","original.frm");
    assert(copied>=0 && sfallFileSystemSize(copied)==8);
    assert(sfallFileSystemFind("ART\\TEST.FRM")==copied);
    sfallFileSystemSeek(copied,2); sfallFileSystemWrite(copied,0xABCD,2);
    File* game=sfallFileSystemOpen("ART/test.frm"); assert(game);
    unsigned char b[8]; assert(fread(b,1,8,game->file)==8);
    assert(b[0]==0 && b[2]==0xAB && b[3]==0xCD && b[4]==4);
    fclose(game->file); free(game);
    assert(sfallFileSystemPos(copied)==4); // engine has an independent cursor
    int text=sfallFileSystemCreate("text.txt",4); assert(text>=0);
    sfallFileSystemWriteString(text,"abc",true); assert(sfallFileSystemPos(text)==4);
    sfallFileSystemWriteString(text,"overflow",true); assert(sfallFileSystemPos(text)==4);
    sfallFileSystemResize(copied,-1); // mark for saving
    File save{}; save.type=XFILE_TYPE_FILE; save.file=tmpfile(); assert(save.file);
    assert(sfallFileSystemSave(&save)); sfallFileSystemReset();
    rewind(save.file); assert(sfallFileSystemLoad(&save)); fclose(save.file);
    assert(sfallFileSystemFind("art/test.frm")==copied && sfallFileSystemPos(copied)==4);
    assert(sfallFileSystemFind("text.txt")==-1); // unmarked files are transient
    sfallFileSystemSeek(copied,2); assert(sfallFileSystemRead(copied,2)==0xABCD);
    sfallFileSystemSeek(copied,100); assert(sfallFileSystemPos(copied)==4);
    sfallFileSystemDelete(copied); assert(sfallFileSystemFind("art/test.frm")==-1);
    sfallFileSystemSeek(-1,0); sfallFileSystemWrite(999,42,4);
    assert(sfallFileSystemRead(-1,4)==0);
    assert(sfallFileSystemCreate("invalid",-1)==-1);
    sfallFileSystemReset();
    File old{}; old.type=XFILE_TYPE_FILE; old.file=tmpfile();
    assert(sfallFileSystemLoad(&old)); fclose(old.file); // old save without trailer
    for (int i=0;i<4096;++i) {
        char name[32]; snprintf(name,sizeof(name),"zero-%d",i);
        assert(sfallFileSystemCreate(name,0)>=0);
    }
    assert(sfallFileSystemCreate("too-many",0)==-1);
    sfallFileSystemReset();
    for (int i=0;i<6;++i) {
        char name[32]; snprintf(name,sizeof(name),"large-%d",i);
        assert(sfallFileSystemCreate(name,10*1024*1024)>=0);
    }
    assert(sfallFileSystemCreate("large-7",10*1024*1024)==-1);
    sfallFileSystemReset();

}
