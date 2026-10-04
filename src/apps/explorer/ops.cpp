// explorer: copying, moving and deleting - files, and folders with all that
// is in them.
//
// A job (BeginJob) runs over many items; what goes wrong with one is asked
// about (skip it, or stop the job), and Esc stops it between the pieces. The
// answers "all" and "none" to "overwrite?" hold for the rest of the job.

#include "explorer.h"

static const char* JobTitle;
static bool     Stop;               // the job was stopped: nothing more is done
static bool     OverwriteAll, OverwriteNone;
static uint32_t Skipped;            // items left as they were

void BeginJob(const char* Title)
{
    JobTitle = Title;
    Stop = OverwriteAll = OverwriteNone = false;
    Skipped = 0;
    Progress(JobTitle, "", -1, true);
}

// Something went wrong with Path: skip it, or stop. False when stopped.
static bool Problem(const char* What, const char* Path, SfStatus Status)
{
    static const char* const Labels[] = { "Skip", "Cancel" };
    char Line[128];
    char* p = Append(Line, What);
    if (SF_ERROR(Status))
    {
        p = Append(p, ": ");
        Append(p, Why(Status));
    }
    Skipped++;
    if (SfButtons(Ui, JobTitle, Line, Path, Labels, 2, true) != 0)
        Stop = true;
    return !Stop;
}

static bool CheckStop()
{
    if (!Stop && SfUiEscape(Ui))
        Stop = true;
    return Stop;
}

// To exists: may it be replaced? False: leave it (or the job was stopped).
static bool MayOverwrite(const char* To)
{
    if (OverwriteAll)
        return true;
    if (!OverwriteNone)
    {
        static const char* const Labels[] = { "Overwrite", "All", "Skip", "None", "Cancel" };
        switch (SfButtons(Ui, JobTitle, "It exists already:", To, Labels, 5, false))
        {
            case 0:  return true;
            case 1:  OverwriteAll = true; return true;
            case 2:  break;
            case 3:  OverwriteNone = true; break;
            default: Stop = true; return false;
        }
    }
    Skipped++;
    return false;
}

// What is at Path: 0 nothing, 1 a file, 2 a folder.
static int Kind(const char* Path)
{
    SfFile* File;
    if (SF_ERROR(Open(Path, SF_FILE_READ, &File)))
        return 0;
    int Result = IsFolder(File) ? 2 : 1;
    File->Close(File);
    return Result;
}

static SfStatus Delete(const char* Path)
{
    char Full[PATH_SIZE + 8];
    DiskPath(Path, Full);
    return Sys->Files->Delete(Sys->Files, Full);
}

static bool CopyFile(SfFile* Src, uint64_t Total, const char* From, const char* To)
{
    int There = Kind(To);
    if (There == 2)
        return Problem("A folder by that name is in the way", To, SF_SUCCESS);
    if (There == 1 && !MayOverwrite(To))
        return !Stop;

    SfFile* Dst;
    SfStatus Status = Open(To, SF_FILE_WRITE | SF_FILE_CREATE | SF_FILE_TRUNCATE, &Dst);
    if (SF_ERROR(Status))
        return Problem("Cannot write the file", To, Status);

    static uint8_t Buffer[65536];
    uint64_t Done = 0;
    while (!SF_ERROR(Status))
    {
        Progress(JobTitle, From, Total ? (int)(Done * 100 / Total) : 100);
        if (CheckStop())
            break;
        uint64_t n = sizeof(Buffer);
        Status = Src->Read(Src, Buffer, &n);
        if (SF_ERROR(Status) || n == 0)
            break;
        uint64_t w = n;
        Status = Dst->Write(Dst, Buffer, &w);
        if (!SF_ERROR(Status) && w != n)
            Status = SF_OUT_OF_RESOURCES;
        Done += n;
    }
    Dst->Close(Dst);
    if (Status == SF_END_OF_FILE)
        Status = SF_SUCCESS;
    if (Stop || SF_ERROR(Status))
        Delete(To);                     // half a file is worse than none
    if (SF_ERROR(Status))
        return Problem("Cannot copy the file", From, Status);
    return !Stop;
}

bool CopyItem(const char* From, const char* To)
{
    if (CheckStop())
        return false;
    if (CompareNoCase(From, To) == 0)       // names differ by case at most: one file
        return Problem("Cannot copy it onto itself", From, SF_SUCCESS);

    SfFile* Src;
    SfStatus Status = Open(From, SF_FILE_READ, &Src);
    if (SF_ERROR(Status))
        return Problem("Cannot open it", From, Status);
    SfDirEntry Info;
    Src->GetInfo(Src, &Info);
    if (!(Info.Flags & SF_DIR_ENTRY_FOLDER))
    {
        bool Ok = CopyFile(Src, Info.Size, From, To);
        Src->Close(Src);
        return Ok;
    }

    if (Inside(To, From))
    {
        Src->Close(Src);
        return Problem("Cannot copy a folder into itself", From, SF_SUCCESS);
    }
    char Full[PATH_SIZE + 8];
    DiskPath(To, Full);
    Status = Sys->Files->CreateDirectory(Sys->Files, Full);
    if (SF_ERROR(Status) && Kind(To) != 2)      // a folder there already: into it
    {
        Src->Close(Src);
        return Problem("Cannot make the folder", To, Status);
    }
    SfDirEntry E;
    while (!Stop && Src->ReadDir(Src, &E) == SF_SUCCESS)
    {
        char A[PATH_SIZE], B[PATH_SIZE];
        if (!Join(A, From, E.Name) || !Join(B, To, E.Name))
            Problem("The path is too long", E.Name, SF_SUCCESS);
        else
            CopyItem(A, B);
    }
    Src->Close(Src);
    return !Stop;
}

// A folder is emptied one entry at a time, the folder closed while the
// entry goes: nothing is deleted under an open listing.
bool DeleteItem(const char* Path)
{
    if (CheckStop())
        return false;
    Progress(JobTitle, Path, -1);

    if (Kind(Path) == 2)
        for (;;)
        {
            SfFile* Dir;
            SfDirEntry E;
            if (SF_ERROR(Open(Path, SF_FILE_READ, &Dir)))
                break;
            SfStatus Status = Dir->ReadDir(Dir, &E);
            Dir->Close(Dir);
            if (Status != SF_SUCCESS)
                break;                  // empty now
            char Child[PATH_SIZE];
            uint32_t Before = Skipped;
            if (!Join(Child, Path, E.Name))
                Problem("The path is too long", E.Name, SF_SUCCESS);
            else
                DeleteItem(Child);
            if (Stop)
                return false;
            if (Skipped != Before)
                return true;            // something stays, so the folder does
        }

    SfStatus Status = Delete(Path);
    if (SF_ERROR(Status))
        return Problem("Cannot delete it", Path, Status);
    return true;
}

// Rename where that is possible; onto another volume it is a copy, and the
// original goes only when all of it arrived.
bool MoveItem(const char* From, const char* To)
{
    if (CheckStop())
        return false;
    if (Same(From, To))
        return true;
    Progress(JobTitle, From, -1);
    if (Kind(From) == 2 && Inside(To, From))
        return Problem("Cannot move a folder into itself", From, SF_SUCCESS);

    // A new name that differs by case only is the same file: nothing is in
    // the way, and nothing may be deleted to make room.
    int There = CompareNoCase(From, To) == 0 ? 0 : Kind(To);
    if (There == 2 && Kind(From) != 2)
        return Problem("A folder by that name is in the way", To, SF_SUCCESS);
    if (There == 1)
    {
        if (!MayOverwrite(To))
            return !Stop;
        Delete(To);
    }

    char A[PATH_SIZE + 8], B[PATH_SIZE + 8];
    DiskPath(From, A);
    DiskPath(To, B);
    SfStatus Status = There == 2 ? SF_ACCESS_DENIED     // folder onto folder: merge
                                 : Sys->Files->Rename(Sys->Files, A, B);
    if (Status == SF_ACCESS_DENIED)
    {
        uint32_t Before = Skipped;
        if (!CopyItem(From, To))
            return false;
        if (Skipped == Before)
            DeleteItem(From);
        return !Stop;
    }
    if (SF_ERROR(Status))
        return Problem("Cannot move it", From, Status);
    return true;
}

// What folder Path holds, everything below it counted. False: stopped.
bool FolderSize(const char* Path, uint64_t* Bytes, uint64_t* Files)
{
    SfFile* Dir;
    if (SF_ERROR(Open(Path, SF_FILE_READ, &Dir)))
        return true;
    Progress("Counting", Path, -1);
    bool Ok = true;
    SfDirEntry E;
    while (Ok && Dir->ReadDir(Dir, &E) == SF_SUCCESS)
    {
        if (E.Flags & SF_DIR_ENTRY_FOLDER)
        {
            char Child[PATH_SIZE];
            if (Join(Child, Path, E.Name))
                Ok = FolderSize(Child, Bytes, Files);
        }
        else
        {
            *Bytes += E.Size;
            (*Files)++;
        }
        if (SfUiEscape(Ui))
            Ok = false;
    }
    Dir->Close(Dir);
    return Ok;
}
