/*
 * recording.c: Recording file handling
 *
 * See the main source file 'vdr.c' for copyright information and
 * how to reach the author.
 *
 * $Id: recording.c 1.162 2008/02/24 10:28:53 kls Exp $
 */

#include "recording.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "channels.h"
#include "i18n.h"
#include "interface.h"
#include "remux.h" //XXX+ I_FRAME
#include "skins.h"
#include "tools.h"
#include "videodir.h"

#define SUMMARYFALLBACK

#define RECEXT       ".rec"
#define DELEXT       ".del"
/* This was the original code, which works fine in a Linux only environment.
   Unfortunately, because of Windows and its brain dead file system, we have
   to use a more complicated approach, in order to allow users who have enabled
   the VFAT compile time option to see their recordings even if they forget to
   enable VFAT when compiling a new version of VDR... Gee, do I hate Windows.
   (kls 2002-07-27)
#define DATAFORMAT   "%4d-%02d-%02d.%02d:%02d.%02d.%02d" RECEXT
#define NAMEFORMAT   "%s/%s/" DATAFORMAT
*/
#define DATAFORMAT   "%4d-%02d-%02d.%02d%*c%02d.%02d.%02d" RECEXT
#define NAMEFORMAT   "%s/%s/" "%4d-%02d-%02d.%02d.%02d.%02d.%02d" RECEXT

#define RESUMEFILESUFFIX  "/resume%s%s.vdr"
#ifdef SUMMARYFALLBACK
#define SUMMARYFILESUFFIX "/summary.vdr"
#endif
#define INFOFILESUFFIX    "/info.vdr"
#define MARKSFILESUFFIX   "/marks.vdr"

#define MINDISKSPACE 1024 // MB

#define REMOVECHECKDELTA   60 // seconds between checks for removing deleted files
#define DELETEDLIFETIME   300 // seconds after which a deleted recording will be actually removed
#define DISKCHECKDELTA    100 // seconds between checks for free disk space
#define REMOVELATENCY      10 // seconds to wait until next check after removing a file

#define TIMERMACRO_TITLE    "TITLE"
#define TIMERMACRO_EPISODE  "EPISODE"

#define MAX_SUBTITLE_LENGTH  40

#define MAX_LINK_LEVEL  6

bool VfatFileSystem = false;

cRecordings DeletedRecordings(true);

// --- cRemoveDeletedRecordingsThread ----------------------------------------

class cRemoveDeletedRecordingsThread : public cThread {
protected:
  virtual void Action(void);
public:
  cRemoveDeletedRecordingsThread(void);
  };

cRemoveDeletedRecordingsThread::cRemoveDeletedRecordingsThread(void)
:cThread("remove deleted recordings")
{
}

void cRemoveDeletedRecordingsThread::Action(void)
{
  SetPriority(19);
  // Make sure only one instance of VDR does this:
  cLockFile LockFile(VideoDirectory);
  if (LockFile.Lock()) {
     bool deleted = false;
     cThreadLock DeletedRecordingsLock(&DeletedRecordings);
     for (cRecording *r = DeletedRecordings.First(); r; ) {
         if (r->deleted && time(NULL) - r->deleted > DELETEDLIFETIME) {
            cRecording *next = DeletedRecordings.Next(r);
            r->Remove();
            DeletedRecordings.Del(r);
            r = next;
            deleted = true;
            continue;
            }
         r = DeletedRecordings.Next(r);
         }
     if (deleted)
        RemoveEmptyVideoDirectories();
     }
}

static cRemoveDeletedRecordingsThread RemoveDeletedRecordingsThread;

// ---

void RemoveDeletedRecordings(void)
{
  static time_t LastRemoveCheck = 0;
  if (time(NULL) - LastRemoveCheck > REMOVECHECKDELTA) {
     if (!RemoveDeletedRecordingsThread.Active()) {
        cThreadLock DeletedRecordingsLock(&DeletedRecordings);
        for (cRecording *r = DeletedRecordings.First(); r; r = DeletedRecordings.Next(r)) {
            if (r->deleted && time(NULL) - r->deleted > DELETEDLIFETIME) {
               RemoveDeletedRecordingsThread.Start();
               break;
               }
            }
        }
     LastRemoveCheck = time(NULL);
     }
}

void AssertFreeDiskSpace(int Priority, bool Force)
{
  static cMutex Mutex;
  cMutexLock MutexLock(&Mutex);
  // With every call to this function we try to actually remove
  // a file, or mark a file for removal ("delete" it), so that
  // it will get removed during the next call.
  static time_t LastFreeDiskCheck = 0;
  int Factor = (Priority == -1) ? 10 : 1;
  if (Force || time(NULL) - LastFreeDiskCheck > DISKCHECKDELTA / Factor) {
     if (!VideoFileSpaceAvailable(MINDISKSPACE)) {
        // Make sure only one instance of VDR does this:
        cLockFile LockFile(VideoDirectory);
        if (!LockFile.Lock())
           return;
        // Remove the oldest file that has been "deleted":
        isyslog("low disk space while recording, trying to remove a deleted recording...");
        cThreadLock DeletedRecordingsLock(&DeletedRecordings);
        if (DeletedRecordings.Count()) {
           cRecording *r = DeletedRecordings.First();
           cRecording *r0 = NULL;
           while (r) {
                 if (IsOnVideoDirectoryFileSystem(r->FileName())) { // only remove recordings that will actually increase the free video disk space
                    if (!r0 || r->start < r0->start)
                       r0 = r;
                    }
                 r = DeletedRecordings.Next(r);
                 }
           if (r0 && r0->Remove()) {
              DeletedRecordings.Del(r0);
              LastFreeDiskCheck += REMOVELATENCY / Factor;
              return;
              }
           }
        else {
           // DeletedRecordings was empty, so to be absolutely sure there are no
           // deleted recordings we need to double check:
           DeletedRecordings.Update(true);
           if (DeletedRecordings.Count())
              return; // the next call will actually remove it
           }
        // No "deleted" files to remove, so let's see if we can delete a recording:
        isyslog("...no deleted recording found, trying to delete an old recording...");
        cThreadLock RecordingsLock(&Recordings);
        if (Recordings.Count()) {
           cRecording *r = Recordings.First();
           cRecording *r0 = NULL;
           while (r) {
                 if (IsOnVideoDirectoryFileSystem(r->FileName())) { // only delete recordings that will actually increase the free video disk space
                    if (!r->IsEdited() && r->lifetime < MAXLIFETIME) { // edited recordings and recordings with MAXLIFETIME live forever
                       if ((r->lifetime == 0 && Priority > r->priority) || // the recording has no guaranteed lifetime and the new recording has higher priority
                           (r->lifetime > 0 && (time(NULL) - r->start) / SECSINDAY >= r->lifetime)) { // the recording's guaranteed lifetime has expired
                          if (r0) {
                             if (r->priority < r0->priority || (r->priority == r0->priority && r->start < r0->start))
                                r0 = r; // in any case we delete the one with the lowest priority (or the older one in case of equal priorities)
                             }
                          else
                             r0 = r;
                          }
                       }
                    }
                 r = Recordings.Next(r);
                 }
           if (r0 && r0->Delete()) {
              Recordings.Del(r0);
              return;
              }
           }
        // Unable to free disk space, but there's nothing we can do about that...
        isyslog("...no old recording found, giving up");
        Skins.QueueMessage(mtWarning, tr("Low disk space!"), 5, -1);
        }
     LastFreeDiskCheck = time(NULL);
     }
}

// --- cResumeFile -----------------------------------------------------------

cResumeFile::cResumeFile(const char *FileName)
{
  fileName = MALLOC(char, strlen(FileName) + strlen(RESUMEFILESUFFIX) + 1);
  if (fileName) {
     strcpy(fileName, FileName);
     sprintf(fileName + strlen(fileName), RESUMEFILESUFFIX, Setup.ResumeID ? "." : "", Setup.ResumeID ? *itoa(Setup.ResumeID) : "");
     }
  else
     esyslog("ERROR: can't allocate memory for resume file name");
}

cResumeFile::~cResumeFile()
{
  free(fileName);
}

int cResumeFile::Read(void)
{
  int resume = -1;
  if (fileName) {
     struct stat st;
     if (stat(fileName, &st) == 0) {
        if ((st.st_mode & S_IWUSR) == 0) // no write access, assume no resume
           return -1;
        }
     int f = open(fileName, O_RDONLY);
     if (f >= 0) {
        if (safe_read(f, &resume, sizeof(resume)) != sizeof(resume)) {
           resume = -1;
           LOG_ERROR_STR(fileName);
           }
        close(f);
        }
     else if (errno != ENOENT)
        LOG_ERROR_STR(fileName);
     }
  return resume;
}

bool cResumeFile::Save(int Index)
{
  if (fileName) {
     int f = open(fileName, O_WRONLY | O_CREAT | O_TRUNC, DEFFILEMODE);
     if (f >= 0) {
        if (safe_write(f, &Index, sizeof(Index)) < 0)
           LOG_ERROR_STR(fileName);
        close(f);
        Recordings.ResetResume(fileName);
        return true;
        }
     }
  return false;
}

void cResumeFile::Delete(void)
{
  if (fileName) {
     if (remove(fileName) < 0 && errno != ENOENT)
        LOG_ERROR_STR(fileName);
     Recordings.ResetResume(fileName);
     }
}

// --- cRecordingInfo --------------------------------------------------------

cRecordingInfo::cRecordingInfo(const cChannel *Channel, const cEvent *Event)
{
  channelID = Channel ? Channel->GetChannelID() : tChannelID::InvalidID;
  channelName = Channel ? strdup(Channel->Name()) : NULL;
  ownEvent = Event ? NULL : new cEvent(0);
  event = ownEvent ? ownEvent : Event;
  aux = NULL;
  if (Channel) {
     // Since the EPG data's component records can carry only a single
     // language code, let's see whether the channel's PID data has
     // more information:
     cComponents *Components = (cComponents *)event->Components();
     if (!Components)
        Components = new cComponents;
     for (int i = 0; i < MAXAPIDS; i++) {
         const char *s = Channel->Alang(i);
         if (*s) {
            tComponent *Component = Components->GetComponent(i, 2, 3);
            if (!Component)
               Components->SetComponent(Components->NumComponents(), 2, 3, s, NULL);
            else if (strlen(s) > strlen(Component->language))
               strn0cpy(Component->language, s, sizeof(Component->language));
            }
         }
     // There's no "multiple languages" for Dolby Digital tracks, but
     // we do the same procedure here, too, in case there is no component
     // information at all:
     for (int i = 0; i < MAXDPIDS; i++) {
         const char *s = Channel->Dlang(i);
         if (*s) {
            tComponent *Component = Components->GetComponent(i, 2, 5);
            if (!Component)
               Components->SetComponent(Components->NumComponents(), 2, 5, s, NULL);
            else if (strlen(s) > strlen(Component->language))
               strn0cpy(Component->language, s, sizeof(Component->language));
            }
         }
     // The same applies to subtitles:
     for (int i = 0; i < MAXSPIDS; i++) {
         const char *s = Channel->Slang(i);
         if (*s) {
            tComponent *Component = Components->GetComponent(i, 3, 3);
            if (!Component)
               Components->SetComponent(Components->NumComponents(), 3, 3, s, NULL);
            else if (strlen(s) > strlen(Component->language))
               strn0cpy(Component->language, s, sizeof(Component->language));
            }
         }
     if (Components != event->Components())
        ((cEvent *)event)->SetComponents(Components);
     }
}

cRecordingInfo::~cRecordingInfo()
{
  delete ownEvent;
  free(aux);
  free(channelName);
}

void cRecordingInfo::SetData(const char *Title, const char *ShortText, const char *Description)
{
  if (!isempty(Title))
     ((cEvent *)event)->SetTitle(Title);
  if (!isempty(ShortText))
     ((cEvent *)event)->SetShortText(ShortText);
  if (!isempty(Description))
     ((cEvent *)event)->SetDescription(Description);
}

void cRecordingInfo::SetAux(const char *Aux)
{
  free(aux);
  aux = Aux ? strdup(Aux) : NULL;
}

bool cRecordingInfo::Read(FILE *f)
{
  if (ownEvent) {
     cReadLine ReadLine;
     char *s;
     int line = 0;
     while ((s = ReadLine.Read(f)) != NULL) {
           ++line;
           char *t = skipspace(s + 1);
           switch (*s) {
             case 'C': {
                         char *p = strchr(t, ' ');
                         if (p) {
                            free(channelName);
                            channelName = strdup(compactspace(p));
                            *p = 0; // strips optional channel name
                            }
                         if (*t)
                            channelID = tChannelID::FromString(t);
                       }
                       break;
             case 'E': {
                         unsigned int EventID;
                         time_t StartTime;
                         int Duration;
                         unsigned int TableID = 0;
                         unsigned int Version = 0xFF;
                         int n = sscanf(t, "%u %ld %d %X %X", &EventID, &StartTime, &Duration, &TableID, &Version);
                         if (n >= 3 && n <= 5) {
                            ownEvent->SetEventID(EventID);
                            ownEvent->SetStartTime(StartTime);
                            ownEvent->SetDuration(Duration);
                            ownEvent->SetTableID(TableID);
                            ownEvent->SetVersion(Version);
                            }
                       }
                       break;
             case '@': free(aux);
                       aux = strdup(t);
                       break;
             case '#': break; // comments are ignored
             default: if (!ownEvent->Parse(s)) {
                         esyslog("ERROR: EPG data problem in line %d", line);
                         return false;
                         }
                      break;
             }
           }
     return true;
     }
  return false;
}

bool cRecordingInfo::Write(FILE *f, const char *Prefix) const
{
  if (channelID.Valid())
     fprintf(f, "%sC %s%s%s\n", Prefix, *channelID.ToString(), channelName ? " " : "", channelName ? channelName : "");
  event->Dump(f, Prefix, true);
  if (aux)
     fprintf(f, "%s@ %s\n", Prefix, aux);
  return true;
}

// --- cRecording ------------------------------------------------------------

#define RESUME_NOT_INITIALIZED (-2)

struct tCharExchange { char a; char b; };
tCharExchange CharExchange[] = {
  { '~',  '/'    },
  { '/',  '~'    },
  { ' ',  '_'    },
  // backwards compatibility:
  { '\'', '\''   },
  { '\'', '\x01' },
  { '/',  '\x02' },
  { 0, 0 }
  };

char *ExchangeChars(char *s, bool ToFileSystem)
{
  char *p = s;
  while (*p) {
        if (VfatFileSystem) {
           // The VFAT file system can't handle all characters, so we
           // have to take extra efforts to encode/decode them:
           if (ToFileSystem) {
              const char *InvalidChars = "\"\\/:*?|<>#";
              switch (*p) {
                     // characters that can be mapped to other characters:
                     case ' ': *p = '_'; break;
                     case '~': *p = '/'; break;
                     // characters that have to be encoded:
                     default:
                       if (strchr(InvalidChars, *p) || *p == '.' && (!*(p + 1) || *(p + 1) == '~')) { // Windows can't handle '.' at the end of file/directory names
                          int l = p - s;
                          s = (char *)realloc(s, strlen(s) + 10);
                          p = s + l;
                          char buf[4];
                          sprintf(buf, "#%02X", (unsigned char)*p);
                          memmove(p + 2, p, strlen(p) + 1);
                          strncpy(p, buf, 3);
                          p += 2;
                          }
                     }
              }
           else {
              switch (*p) {
                // mapped characters:
                case '_': *p = ' '; break;
                case '/': *p = '~'; break;
                // encoded characters:
                case '#': {
                     if (strlen(p) > 2 && isxdigit(*(p + 1)) && isxdigit(*(p + 2))) {
                        char buf[3];
                        sprintf(buf, "%c%c", *(p + 1), *(p + 2));
                        unsigned char c = strtol(buf, NULL, 16);
                        if (c) {
                           *p = c;
                           memmove(p + 1, p + 3, strlen(p) - 2);
                           }
                        }
                     }
                     break;
                // backwards compatibility:
                case '\x01': *p = '\''; break;
                case '\x02': *p = '/';  break;
                case '\x03': *p = ':';  break;
                }
              }
           }
        else {
           for (struct tCharExchange *ce = CharExchange; ce->a && ce->b; ce++) {
               if (*p == (ToFileSystem ? ce->a : ce->b)) {
                  *p = ToFileSystem ? ce->b : ce->a;
                  break;
                  }
               }
           }
        p++;
        }
  return s;
}

cRecording::cRecording(cTimer *Timer, const cEvent *Event)
{
  resume = RESUME_NOT_INITIALIZED;
  titleBuffer = NULL;
  sortBuffer = NULL;
  fileName = NULL;
  name = NULL;
  fileSizeMB = -1; // unknown
  deleted = 0;
  // set up the actual name:
  const char *Title = Event ? Event->Title() : NULL;
  const char *Subtitle = Event ? Event->ShortText() : NULL;
  char SubtitleBuffer[MAX_SUBTITLE_LENGTH];
  if (isempty(Title))
     Title = Timer->Channel()->Name();
  if (isempty(Subtitle))
     Subtitle = " ";
  else if (strlen(Subtitle) > MAX_SUBTITLE_LENGTH) {
     // let's make sure the Subtitle doesn't produce too long a file name:
     Utf8Strn0Cpy(SubtitleBuffer, Subtitle, MAX_SUBTITLE_LENGTH);
     Subtitle = SubtitleBuffer;
     }
  const char *macroTITLE   = strstr(Timer->File(), TIMERMACRO_TITLE);
  const char *macroEPISODE = strstr(Timer->File(), TIMERMACRO_EPISODE);
  if (macroTITLE || macroEPISODE) {
     name = strdup(Timer->File());
     name = strreplace(name, TIMERMACRO_TITLE, Title);
     name = strreplace(name, TIMERMACRO_EPISODE, Subtitle);
     // avoid blanks at the end:
     int l = strlen(name);
     while (l-- > 2) {
           if (name[l] == ' ' && name[l - 1] != '~')
              name[l] = 0;
           else
              break;
           }
     if (Timer->IsSingleEvent()) {
        Timer->SetFile(name); // this was an instant recording, so let's set the actual data
        Timers.SetModified();
        }
     }
  else if (Timer->IsSingleEvent() || !Setup.UseSubtitle)
     name = strdup(Timer->File());
  else
     name = strdup(cString::sprintf("%s~%s", Timer->File(), Subtitle));
  // substitute characters that would cause problems in file names:
  strreplace(name, '\n', ' ');
  start = Timer->StartTime();
  priority = Timer->Priority();
  lifetime = Timer->Lifetime();
  // handle info:
  info = new cRecordingInfo(Timer->Channel(), Event);
  info->SetAux(Timer->Aux());
}

cRecording::cRecording(const char *FileName)
{
  resume = RESUME_NOT_INITIALIZED;
  fileSizeMB = -1; // unknown
  deleted = 0;
  titleBuffer = NULL;
  sortBuffer = NULL;
  fileName = strdup(FileName);
  FileName += strlen(VideoDirectory) + 1;
  const char *p = strrchr(FileName, '/');

  name = NULL;
  info = new cRecordingInfo;
  if (p) {
     time_t now = time(NULL);
     struct tm tm_r;
     struct tm t = *localtime_r(&now, &tm_r); // this initializes the time zone in 't'
     t.tm_isdst = -1; // makes sure mktime() will determine the correct DST setting
     if (7 == sscanf(p + 1, DATAFORMAT, &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &priority, &lifetime)) {
        t.tm_year -= 1900;
        t.tm_mon--;
        t.tm_sec = 0;
        start = mktime(&t);
        name = MALLOC(char, p - FileName + 1);
        strncpy(name, FileName, p - FileName);
        name[p - FileName] = 0;
        name = ExchangeChars(name, false);
        }
     GetResume();
     // read an optional info file:
     cString InfoFileName = cString::sprintf("%s%s", fileName, INFOFILESUFFIX);
     FILE *f = fopen(InfoFileName, "r");
     if (f) {
        if (!info->Read(f))
           esyslog("ERROR: EPG data problem in file %s", *InfoFileName);
        fclose(f);
        }
     else if (errno != ENOENT)
        LOG_ERROR_STR(*InfoFileName);
#ifdef SUMMARYFALLBACK
     // fall back to the old 'summary.vdr' if there was no 'info.vdr':
     if (isempty(info->Title())) {
        cString SummaryFileName = cString::sprintf("%s%s", fileName, SUMMARYFILESUFFIX);
        FILE *f = fopen(SummaryFileName, "r");
        if (f) {
           int line = 0;
           char *data[3] = { NULL };
           cReadLine ReadLine;
           char *s;
           while ((s = ReadLine.Read(f)) != NULL) {
                 if (*s || line > 1) {
                    if (data[line]) {
                       int len = strlen(s);
                       len += strlen(data[line]) + 1;
                       data[line] = (char *)realloc(data[line], len + 1);
                       strcat(data[line], "\n");
                       strcat(data[line], s);
                       }
                    else
                       data[line] = strdup(s);
                    }
                 else
                    line++;
                 }
           fclose(f);
           if (!data[2]) {
              data[2] = data[1];
              data[1] = NULL;
              }
           else if (data[1] && data[2]) {
              // if line 1 is too long, it can't be the short text,
              // so assume the short text is missing and concatenate
              // line 1 and line 2 to be the long text:
              int len = strlen(data[1]);
              if (len > 80) {
                 data[1] = (char *)realloc(data[1], len + 1 + strlen(data[2]) + 1);
                 strcat(data[1], "\n");
                 strcat(data[1], data[2]);
                 free(data[2]);
                 data[2] = data[1];
                 data[1] = NULL;
                 }
              }
           info->SetData(data[0], data[1], data[2]);
           for (int i = 0; i < 3; i ++)
               free(data[i]);
           }
        else if (errno != ENOENT)
           LOG_ERROR_STR(*SummaryFileName);
        }
#endif
     }
}

cRecording::~cRecording()
{
  free(titleBuffer);
  free(sortBuffer);
  free(fileName);
  free(name);
  delete info;
}

char *cRecording::StripEpisodeName(char *s)
{
  char *t = s, *s1 = NULL, *s2 = NULL;
  while (*t) {
        if (*t == '/') {
           if (s1) {
              if (s2)
                 s1 = s2;
              s2 = t;
              }
           else
              s1 = t;
           }
        t++;
        }
  if (s1 && s2)
     memmove(s1 + 1, s2, t - s2 + 1);
  return s;
}

char *cRecording::SortName(void) const
{
  if (!sortBuffer) {
     char *s = StripEpisodeName(strdup(FileName() + strlen(VideoDirectory) + 1));
     strreplace(s, '/', 'a'); // some locales ignore '/' when sorting
     int l = strxfrm(NULL, s, 0) + 1;
     sortBuffer = MALLOC(char, l);
     strxfrm(sortBuffer, s, l);
     free(s);
     }
  return sortBuffer;
}

int cRecording::GetResume(void) const
{
  if (resume == RESUME_NOT_INITIALIZED) {
     cResumeFile ResumeFile(FileName());
     resume = ResumeFile.Read();
     }
  return resume;
}

int cRecording::Compare(const cListObject &ListObject) const
{
  cRecording *r = (cRecording *)&ListObject;
  return strcasecmp(SortName(), r->SortName());
}

const char *cRecording::FileName(void) const
{
  if (!fileName) {
     struct tm tm_r;
     struct tm *t = localtime_r(&start, &tm_r);
     name = ExchangeChars(name, true);
     fileName = strdup(cString::sprintf(NAMEFORMAT, VideoDirectory, name, t->tm_year + 1900, t->tm_mon + 1, t->tm_mday, t->tm_hour, t->tm_min, priority, lifetime));
     name = ExchangeChars(name, false);
     }
  return fileName;
}

const char *cRecording::Title(char Delimiter, bool NewIndicator, int Level) const
{
  char New = NewIndicator && IsNew() ? '*' : ' ';
  free(titleBuffer);
  titleBuffer = NULL;
  if (Level < 0 || Level == HierarchyLevels()) {
     struct tm tm_r;
     struct tm *t = localtime_r(&start, &tm_r);
     char *s;
     if (Level > 0 && (s = strrchr(name, '~')) != NULL)
        s++;
     else
        s = name;
     titleBuffer = strdup(cString::sprintf("%02d.%02d.%02d%c%02d:%02d%c%c%s",
                            t->tm_mday,
                            t->tm_mon + 1,
                            t->tm_year % 100,
                            Delimiter,
                            t->tm_hour,
                            t->tm_min,
                            New,
                            Delimiter,
                            s));
     // let's not display a trailing '~':
     if (!NewIndicator)
        stripspace(titleBuffer);
     s = &titleBuffer[strlen(titleBuffer) - 1];
     if (*s == '~')
        *s = 0;
     }
  else if (Level < HierarchyLevels()) {
     const char *s = name;
     const char *p = s;
     while (*++s) {
           if (*s == '~') {
              if (Level--)
                 p = s + 1;
              else
                 break;
              }
           }
     titleBuffer = MALLOC(char, s - p + 3);
     *titleBuffer = Delimiter;
     *(titleBuffer + 1) = Delimiter;
     strn0cpy(titleBuffer + 2, p, s - p + 1);
     }
  else
     return "";
  return titleBuffer;
}

const char *cRecording::PrefixFileName(char Prefix)
{
  cString p = PrefixVideoFileName(FileName(), Prefix);
  if (*p) {
     free(fileName);
     fileName = strdup(p);
     return fileName;
     }
  return NULL;
}

int cRecording::HierarchyLevels(void) const
{
  const char *s = name;
  int level = 0;
  while (*++s) {
        if (*s == '~')
           level++;
        }
  return level;
}

bool cRecording::IsEdited(void) const
{
  const char *s = strrchr(name, '~');
  s = !s ? name : s + 1;
  return *s == '%';
}

bool cRecording::WriteInfo(void)
{
  cString InfoFileName = cString::sprintf("%s%s", fileName, INFOFILESUFFIX);
  FILE *f = fopen(InfoFileName, "w");
  if (f) {
     info->Write(f);
     fclose(f);
     }
  else
     LOG_ERROR_STR(*InfoFileName);
  return true;
}

bool cRecording::Delete(void)
{
  bool result = true;
  char *NewName = strdup(FileName());
  char *ext = strrchr(NewName, '.');
  if (ext && strcmp(ext, RECEXT) == 0) {
     strncpy(ext, DELEXT, strlen(ext));
     if (access(NewName, F_OK) == 0) {
        // the new name already exists, so let's remove that one first:
        isyslog("removing recording '%s'", NewName);
        RemoveVideoFile(NewName);
        }
     isyslog("deleting recording '%s'", FileName());
     if (access(FileName(), F_OK) == 0)
        result = RenameVideoFile(FileName(), NewName);
     else {
        isyslog("recording '%s' vanished", FileName());
        result = true; // well, we were going to delete it, anyway
        }
     }
  free(NewName);
  return result;
}

bool cRecording::Remove(void)
{
  // let's do a final safety check here:
  if (!endswith(FileName(), DELEXT)) {
     esyslog("attempt to remove recording %s", FileName());
     return false;
     }
  isyslog("removing recording %s", FileName());
  return RemoveVideoFile(FileName());
}

bool cRecording::Undelete(void)
{
  bool result = true;
  char *NewName = strdup(FileName());
  char *ext = strrchr(NewName, '.');
  if (ext && strcmp(ext, DELEXT) == 0) {
     strncpy(ext, RECEXT, strlen(ext));
     if (access(NewName, F_OK) == 0) {
        // the new name already exists, so let's not remove that one:
        esyslog("ERROR: attempt to undelete '%s', while recording '%s' exists", FileName(), NewName);
        result = false;
        }
     else {
        isyslog("undeleting recording '%s'", FileName());
        if (access(FileName(), F_OK) == 0)
           result = RenameVideoFile(FileName(), NewName);
        else {
           isyslog("deleted recording '%s' vanished", FileName());
           result = false;
           }
        }
     }
  free(NewName);
  return result;
}

void cRecording::ResetResume(void) const
{
  resume = RESUME_NOT_INITIALIZED;
}

// --- cRecordings -----------------------------------------------------------

cRecordings Recordings;

char *cRecordings::updateFileName = NULL;

cRecordings::cRecordings(bool Deleted)
:cThread("video directory scanner")
{
  deleted = Deleted;
  lastUpdate = 0;
  state = 0;
}

cRecordings::~cRecordings()
{
  Cancel(3);
}

void cRecordings::Action(void)
{
  Refresh();
}

const char *cRecordings::UpdateFileName(void)
{
  if (!updateFileName)
     updateFileName = strdup(AddDirectory(VideoDirectory, ".update"));
  return updateFileName;
}

void cRecordings::Refresh(bool Foreground)
{
  lastUpdate = time(NULL); // doing this first to make sure we don't miss anything
  Lock();
  Clear();
  ChangeState();
  Unlock();
  ScanVideoDir(VideoDirectory, Foreground);
}

void cRecordings::ScanVideoDir(const char *DirName, bool Foreground, int LinkLevel)
{
  cReadDir d(DirName);
  struct dirent *e;
  while ((Foreground || Running()) && (e = d.Next()) != NULL) {
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
           char *buffer = strdup(AddDirectory(DirName, e->d_name));
           struct stat st;
           if (stat(buffer, &st) == 0) {
              int Link = 0;
              if (S_ISLNK(st.st_mode)) {
                 if (LinkLevel > MAX_LINK_LEVEL) {
                    isyslog("max link level exceeded - not scanning %s", buffer);
                    continue;
                    }
                 Link = 1;
                 char *old = buffer;
                 buffer = ReadLink(old);
                 free(old);
                 if (!buffer)
                    continue;
                 if (stat(buffer, &st) != 0) {
                    free(buffer);
                    continue;
                    }
                 }
              if (S_ISDIR(st.st_mode)) {
                 if (endswith(buffer, deleted ? DELEXT : RECEXT)) {
                    cRecording *r = new cRecording(buffer);
                    if (r->Name()) {
                       Lock();
                       Add(r);
                       ChangeState();
                       Unlock();
                       if (deleted) {
                          r->fileSizeMB = DirSizeMB(buffer);
                          r->deleted = time(NULL);
                          }
                       }
                    else
                       delete r;
                    }
                 else
                    ScanVideoDir(buffer, Foreground, LinkLevel + Link);
                 }
              }
           free(buffer);
           }
        }
}

bool cRecordings::StateChanged(int &State)
{
  int NewState = state;
  bool Result = State != NewState;
  State = state;
  return Result;
}

void cRecordings::TouchUpdate(void)
{
  bool needsUpdate = NeedsUpdate();
  TouchFile(UpdateFileName());
  if (!needsUpdate)
     lastUpdate = time(NULL); // make sure we don't tigger ourselves
}

bool cRecordings::NeedsUpdate(void)
{
  time_t lastModified = LastModifiedTime(UpdateFileName());
  if (lastModified > time(NULL))
     return false; // somebody's clock isn't running correctly
  return lastUpdate < lastModified;
}

bool cRecordings::Update(bool Wait)
{
  if (Wait) {
     Refresh(true);
     return Count() > 0;
     }
  else
     Start();
  return false;
}

cRecording *cRecordings::GetByName(const char *FileName)
{
  if (FileName) {
     for (cRecording *recording = First(); recording; recording = Next(recording)) {
         if (strcmp(recording->FileName(), FileName) == 0)
            return recording;
         }
     }
  return NULL;
}

void cRecordings::AddByName(const char *FileName, bool TriggerUpdate)
{
  LOCK_THREAD;
  cRecording *recording = GetByName(FileName);
  if (!recording) {
     recording = new cRecording(FileName);
     Add(recording);
     ChangeState();
     if (TriggerUpdate)
        TouchUpdate();
     }
}

void cRecordings::DelByName(const char *FileName)
{
  LOCK_THREAD;
  cRecording *recording = GetByName(FileName);
  if (recording) {
     cThreadLock DeletedRecordingsLock(&DeletedRecordings);
     Del(recording, false);
     char *ext = strrchr(recording->fileName, '.');
     if (ext) {
        strncpy(ext, DELEXT, strlen(ext));
        recording->fileSizeMB = DirSizeMB(recording->FileName());
        recording->deleted = time(NULL);
        DeletedRecordings.Add(recording);
        }
     else
        delete recording;
     ChangeState();
     TouchUpdate();
     }
}

int cRecordings::TotalFileSizeMB(void)
{
  int size = 0;
  LOCK_THREAD;
  for (cRecording *recording = First(); recording; recording = Next(recording)) {
      if (recording->fileSizeMB > 0 && IsOnVideoDirectoryFileSystem(recording->FileName()))
         size += recording->fileSizeMB;
      }
  return size;
}

void cRecordings::ResetResume(const char *ResumeFileName)
{
  LOCK_THREAD;
  for (cRecording *recording = First(); recording; recording = Next(recording)) {
      if (!ResumeFileName || strncmp(ResumeFileName, recording->FileName(), strlen(recording->FileName())) == 0)
         recording->ResetResume();
      }
  ChangeState();
}

// --- cMark -----------------------------------------------------------------

cMark::cMark(int Position, const char *Comment)
{
  position = Position;
  comment = Comment ? strdup(Comment) : NULL;
}

cMark::~cMark()
{
  free(comment);
}

cString cMark::ToText(void)
{
  return cString::sprintf("%s%s%s\n", *IndexToHMSF(position, true), comment ? " " : "", comment ? comment : "");
}

bool cMark::Parse(const char *s)
{
  free(comment);
  comment = NULL;
  position = HMSFToIndex(s);
  const char *p = strchr(s, ' ');
  if (p) {
     p = skipspace(p);
     if (*p)
        comment = strdup(p);
     }
  return true;
}

bool cMark::Save(FILE *f)
{
  return fprintf(f, "%s", *ToText()) > 0;
}

// --- cMarks ----------------------------------------------------------------

bool cMarks::Load(const char *RecordingFileName)
{
  if (cConfig<cMark>::Load(AddDirectory(RecordingFileName, MARKSFILESUFFIX))) {
     Sort();
     return true;
     }
  return false;
}

void cMarks::Sort(void)
{
  for (cMark *m1 = First(); m1; m1 = Next(m1)) {
      for (cMark *m2 = Next(m1); m2; m2 = Next(m2)) {
          if (m2->position < m1->position) {
             swap(m1->position, m2->position);
             swap(m1->comment, m2->comment);
             }
          }
      }
}

cMark *cMarks::Add(int Position)
{
  cMark *m = Get(Position);
  if (!m) {
     cConfig<cMark>::Add(m = new cMark(Position));
     Sort();
     }
  return m;
}

cMark *cMarks::Get(int Position)
{
  for (cMark *mi = First(); mi; mi = Next(mi)) {
      if (mi->position == Position)
         return mi;
      }
  return NULL;
}

cMark *cMarks::GetPrev(int Position)
{
  for (cMark *mi = Last(); mi; mi = Prev(mi)) {
      if (mi->position < Position)
         return mi;
      }
  return NULL;
}

cMark *cMarks::GetNext(int Position)
{
  for (cMark *mi = First(); mi; mi = Next(mi)) {
      if (mi->position > Position)
         return mi;
      }
  return NULL;
}

// --- cRecordingUserCommand -------------------------------------------------

const char *cRecordingUserCommand::command = NULL;

void cRecordingUserCommand::InvokeCommand(const char *State, const char *RecordingFileName)
{
  if (command) {
     cString cmd = cString::sprintf("%s %s \"%s\"", command, State, *strescape(RecordingFileName, "\\\"$"));
     isyslog("executing '%s'", *cmd);
     SystemExec(cmd);
     }
}

// --- XXX+

//XXX+ somewhere else???
// --- cIndexFile ------------------------------------------------------------

#define INDEXFILESUFFIX     "/index.vdr"

// The number of frames to stay off the end in case of time shift:
#define INDEXSAFETYLIMIT 150 // frames

// The maximum time to wait before giving up while catching up on an index file:
#define MAXINDEXCATCHUP   8 // seconds

// The minimum age of an index file for considering it no longer to be written:
#define MININDEXAGE    3600 // seconds

cIndexFile::cIndexFile(const char *FileName, bool Record)
:resumeFile(FileName)
{
  f = -1;
  fileName = NULL;
  size = 0;
  last = -1;
  index = NULL;
  if (FileName) {
     fileName = MALLOC(char, strlen(FileName) + strlen(INDEXFILESUFFIX) + 1);
     if (fileName) {
        strcpy(fileName, FileName);
        char *pFileExt = fileName + strlen(fileName);
        strcpy(pFileExt, INDEXFILESUFFIX);
        int delta = 0;
        if (access(fileName, R_OK) == 0) {
           struct stat buf;
           if (stat(fileName, &buf) == 0) {
              delta = buf.st_size % sizeof(tIndex);
              if (delta) {
                 delta = sizeof(tIndex) - delta;
                 esyslog("ERROR: invalid file size (%ld) in '%s'", buf.st_size, fileName);
                 }
              last = (buf.st_size + delta) / sizeof(tIndex) - 1;
              if (!Record && last >= 0) {
                 size = last + 1;
                 index = MALLOC(tIndex, size);
                 if (index) {
                    f = open(fileName, O_RDONLY);
                    if (f >= 0) {
                       if ((int)safe_read(f, index, buf.st_size) != buf.st_size) {
                          esyslog("ERROR: can't read from file '%s'", fileName);
                          free(index);
                          index = NULL;
                          close(f);
                          f = -1;
                          }
                       // we don't close f here, see CatchUp()!
                       }
                    else
                       LOG_ERROR_STR(fileName);
                    }
                 else
                    esyslog("ERROR: can't allocate %zd bytes for index '%s'", size * sizeof(tIndex), fileName);
                 }
              }
           else
              LOG_ERROR;
           }
        else if (!Record)
           isyslog("missing index file %s", fileName);
        if (Record) {
           if ((f = open(fileName, O_WRONLY | O_CREAT | O_APPEND, DEFFILEMODE)) >= 0) {
              if (delta) {
                 esyslog("ERROR: padding index file with %d '0' bytes", delta);
                 while (delta--)
                       writechar(f, 0);
                 }
              }
           else
              LOG_ERROR_STR(fileName);
           }
        }
     else
        esyslog("ERROR: can't copy file name '%s'", FileName);
     }
}

cIndexFile::~cIndexFile()
{
  if (f >= 0)
     close(f);
  free(fileName);
  free(index);
}

bool cIndexFile::CatchUp(int Index)
{
  // returns true unless something really goes wrong, so that 'index' becomes NULL
  if (index && f >= 0) {
     cMutexLock MutexLock(&mutex);
     for (int i = 0; i <= MAXINDEXCATCHUP && (Index < 0 || Index >= last); i++) {
         struct stat buf;
         if (fstat(f, &buf) == 0) {
            if (time(NULL) - buf.st_mtime > MININDEXAGE) {
               // apparently the index file is not being written any more
               close(f);
               f = -1;
               break;
               }
            int newLast = buf.st_size / sizeof(tIndex) - 1;
            if (newLast > last) {
               if (size <= newLast) {
                  size *= 2;
                  if (size <= newLast)
                     size = newLast + 1;
                  }
               index = (tIndex *)realloc(index, size * sizeof(tIndex));
               if (index) {
                  int offset = (last + 1) * sizeof(tIndex);
                  int delta = (newLast - last) * sizeof(tIndex);
                  if (lseek(f, offset, SEEK_SET) == offset) {
                     if (safe_read(f, &index[last + 1], delta) != delta) {
                        esyslog("ERROR: can't read from index");
                        free(index);
                        index = NULL;
                        close(f);
                        f = -1;
                        break;
                        }
                     last = newLast;
                     }
                  else
                     LOG_ERROR_STR(fileName);
                  }
               else
                  esyslog("ERROR: can't realloc() index");
               }
            }
         else
            LOG_ERROR_STR(fileName);
         if (Index < last - (i ? 2 * INDEXSAFETYLIMIT : 0) || Index > 10 * INDEXSAFETYLIMIT) // keep off the end in case of "Pause live video"
            break;
         cCondWait::SleepMs(1000);
         }
     }
  return index != NULL;
}

bool cIndexFile::Write(uchar PictureType, uchar FileNumber, int FileOffset)
{
  if (f >= 0) {
     tIndex i = { FileOffset, PictureType, FileNumber, 0 };
     if (safe_write(f, &i, sizeof(i)) < 0) {
        LOG_ERROR_STR(fileName);
        close(f);
        f = -1;
        return false;
        }
     last++;
     }
  return f >= 0;
}

bool cIndexFile::Get(int Index, uchar *FileNumber, int *FileOffset, uchar *PictureType, int *Length)
{
  if (CatchUp(Index)) {
     if (Index >= 0 && Index < last) {
        *FileNumber = index[Index].number;
        *FileOffset = index[Index].offset;
        if (PictureType)
           *PictureType = index[Index].type;
        if (Length) {
           int fn = index[Index + 1].number;
           int fo = index[Index + 1].offset;
           if (fn == *FileNumber)
              *Length = fo - *FileOffset;
           else
              *Length = -1; // this means "everything up to EOF" (the buffer's Read function will act accordingly)
           }
        return true;
        }
     }
  return false;
}

int cIndexFile::GetNextIFrame(int Index, bool Forward, uchar *FileNumber, int *FileOffset, int *Length, bool StayOffEnd)
{
  if (CatchUp()) {
     int d = Forward ? 1 : -1;
     for (;;) {
         Index += d;
         if (Index >= 0 && Index < last - ((Forward && StayOffEnd) ? INDEXSAFETYLIMIT : 0)) {
            if (index[Index].type == I_FRAME) {
               if (FileNumber)
                  *FileNumber = index[Index].number;
               else
                  FileNumber = &index[Index].number;
               if (FileOffset)
                  *FileOffset = index[Index].offset;
               else
                  FileOffset = &index[Index].offset;
               if (Length) {
                  // all recordings end with a non-I_FRAME, so the following should be safe:
                  int fn = index[Index + 1].number;
                  int fo = index[Index + 1].offset;
                  if (fn == *FileNumber)
                     *Length = fo - *FileOffset;
                  else {
                     esyslog("ERROR: 'I' frame at end of file #%d", *FileNumber);
                     *Length = -1;
                     }
                  }
               return Index;
               }
            }
         else
            break;
         }
     }
  return -1;
}

int cIndexFile::Get(uchar FileNumber, int FileOffset)
{
  if (CatchUp()) {
     //TODO implement binary search!
     int i;
     for (i = 0; i < last; i++) {
         if (index[i].number > FileNumber || (index[i].number == FileNumber) && index[i].offset >= FileOffset)
            break;
         }
     return i;
     }
  return -1;
}

bool cIndexFile::IsStillRecording()
{
  return f >= 0;
}

// --- cFileName -------------------------------------------------------------

#define MAXFILESPERRECORDING 255
#define RECORDFILESUFFIX    "/%03d.vdr"
#define RECORDFILESUFFIXLEN 20 // some additional bytes for safety...

cFileName::cFileName(const char *FileName, bool Record, bool Blocking)
{
  file = NULL;
  fileNumber = 0;
  record = Record;
  blocking = Blocking;
  // Prepare the file name:
  fileName = MALLOC(char, strlen(FileName) + RECORDFILESUFFIXLEN);
  if (!fileName) {
     esyslog("ERROR: can't copy file name '%s'", fileName);
     return;
     }
  strcpy(fileName, FileName);
  pFileNumber = fileName + strlen(fileName);
  SetOffset(1);
}

cFileName::~cFileName()
{
  Close();
  free(fileName);
}

cUnbufferedFile *cFileName::Open(void)
{
  if (!file) {
     int BlockingFlag = blocking ? 0 : O_NONBLOCK;
     if (record) {
        dsyslog("recording to '%s'", fileName);
        file = OpenVideoFile(fileName, O_RDWR | O_CREAT | BlockingFlag);
        if (!file)
           LOG_ERROR_STR(fileName);
        }
     else {
        if (access(fileName, R_OK) == 0) {
           dsyslog("playing '%s'", fileName);
           file = cUnbufferedFile::Create(fileName, O_RDONLY | BlockingFlag);
           if (!file)
              LOG_ERROR_STR(fileName);
           }
        else if (errno != ENOENT)
           LOG_ERROR_STR(fileName);
        }
     }
  return file;
}

void cFileName::Close(void)
{
  if (file) {
     if (CloseVideoFile(file) < 0)
        LOG_ERROR_STR(fileName);
     file = NULL;
     }
}

cUnbufferedFile *cFileName::SetOffset(int Number, int Offset)
{
  if (fileNumber != Number)
     Close();
  if (0 < Number && Number <= MAXFILESPERRECORDING) {
     fileNumber = Number;
     sprintf(pFileNumber, RECORDFILESUFFIX, fileNumber);
     if (record) {
        if (access(fileName, F_OK) == 0) {
           // files exists, check if it has non-zero size
           struct stat buf;
           if (stat(fileName, &buf) == 0) {
              if (buf.st_size != 0)
                 return SetOffset(Number + 1); // file exists and has non zero size, let's try next suffix
              else {
                 // zero size file, remove it
                 dsyslog ("cFileName::SetOffset: removing zero-sized file %s", fileName);
                 unlink (fileName);
                 }
              }
           else
              return SetOffset(Number + 1); // error with fstat - should not happen, just to be on the safe side
           }
        else if (errno != ENOENT) { // something serious has happened
           LOG_ERROR_STR(fileName);
           return NULL;
           }
        // found a non existing file suffix
        }
     if (Open() >= 0) {
        if (!record && Offset >= 0 && file && file->Seek(Offset, SEEK_SET) != Offset) {
           LOG_ERROR_STR(fileName);
           return NULL;
           }
        }
     return file;
     }
  esyslog("ERROR: max number of files (%d) exceeded", MAXFILESPERRECORDING);
  return NULL;
}

cUnbufferedFile *cFileName::NextFile(void)
{
  return SetOffset(fileNumber + 1);
}

// --- Index stuff -----------------------------------------------------------

cString IndexToHMSF(int Index, bool WithFrame)
{
  char buffer[16];
  int f = (Index % FRAMESPERSEC) + 1;
  int s = (Index / FRAMESPERSEC);
  int m = s / 60 % 60;
  int h = s / 3600;
  s %= 60;
  snprintf(buffer, sizeof(buffer), WithFrame ? "%d:%02d:%02d.%02d" : "%d:%02d:%02d", h, m, s, f);
  return buffer;
}

int HMSFToIndex(const char *HMSF)
{
  int h, m, s, f = 0;
  if (3 <= sscanf(HMSF, "%d:%d:%d.%d", &h, &m, &s, &f))
     return (h * 3600 + m * 60 + s) * FRAMESPERSEC + f - 1;
  return 0;
}

int SecondsToFrames(int Seconds)
{
  return Seconds * FRAMESPERSEC;
}

// --- ReadFrame -------------------------------------------------------------

int ReadFrame(cUnbufferedFile *f, uchar *b, int Length, int Max)
{
  if (Length == -1)
     Length = Max; // this means we read up to EOF (see cIndex)
  else if (Length > Max) {
     esyslog("ERROR: frame larger than buffer (%d > %d)", Length, Max);
     Length = Max;
     }
  int r = f->Read(b, Length);
  if (r < 0)
     LOG_ERROR;
  return r;
}
