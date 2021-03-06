//copyright 'Spike', license gplv2+
//provides both a package manager and downloads menu.
#include "quakedef.h"

#if defined(WEBCLIENT) && !defined(NOBUILTINMENUS)
#define DOWNLOADMENU
#endif

#ifdef DOWNLOADMENU
#include "fs.h"

//whole load of extra args for the downloads menu (for the downloads menu to handle engine updates).
#ifdef VKQUAKE
#define PHPVK "&vk=1"
#else
#define PHPVK
#endif
#ifdef GLQUAKE
#define PHPGL "&gl=1"
#else
#define PHPGL
#endif
#ifdef D3DQUAKE
#define PHPD3D "&d3d=1"
#else
#define PHPD3D
#endif
#ifdef MINIMAL
#define PHPMIN "&min=1"
#else
#define PHPMIN
#endif
#ifdef NOLEGACY
#define PHPLEG "&leg=0"
#else
#define PHPLEG "&leg=1"
#endif
#if defined(_DEBUG) || defined(DEBUG)
#define PHPDBG "&dbg=1"
#else
#define PHPDBG
#endif
#ifndef SVNREVISION
#define SVNREVISION -
#endif
#define DOWNLOADABLESARGS "?ver=" STRINGIFY(SVNREVISION) PHPVK PHPGL PHPD3D PHPMIN PHPLEG PHPDBG "&arch="PLATFORM "_" ARCH_CPU_POSTFIX



extern cvar_t fs_downloads_url;
#define INSTALLEDFILES	"installed.lst"	//the file that resides in the quakedir (saying what's installed).

//installed native okay [previously manually installed, or has no a qhash]
//installed cached okay [had a qhash]
//installed native corrupt [they overwrote it manually]
//installed cached corrupt [we fucked up, probably]
//installed native missing (becomes not installed) [deleted]
//installed cached missing (becomes not installed) [deleted]
//installed none [meta package with no files]

//!installed native okay [was manually installed, flag as installed now]
//!installed cached okay [they got it from some other source / previously installed]
//!installed native corrupt [manually installed conflict]
//!installed cached corrupt [we fucked up, probably]

//!installed * missing [simply not installed]

#define DPF_INSTALLED				0x01
#define DPF_NATIVE					0x02	//appears to be installed properly
#define DPF_CACHED					0x04	//appears to be installed in their dlcache dir (and has a qhash)
#define DPF_CORRUPT					0x08	//will be deleted before it can be changed

#define DPF_MARKED					0x10	//user selected it
#define DPF_DISPLAYVERSION			0x20	//some sort of conflict, the package is listed twice, so show versions so the user knows what's old.
#define DPF_FORGETONUNINSTALL		0x40	//for previously installed packages, remove them from the list if there's no current version any more (should really be automatic if there's no known mirrors)
#define DPF_HIDDEN					0x80	//wrong arch, file conflicts, etc. still listed if actually installed.
#define DPF_ENGINE					0x100	//engine update. replaces old autoupdate mechanism
#define DPF_PURGE					0x200	//package should be completely removed (ie: the dlcache dir too). if its still marked then it should be reinstalled anew. available on cached or corrupt packages, implied by native.

//pak.lst
//priories <0
//pakX
//manifest packages
//priority 0-999
//*.pak
//priority >=1000
#define PM_DEFAULTPRIORITY		1000

void CL_StartCinematicOrMenu(void);

#if defined(SERVERONLY)
#	define ENGINE_RENDERER "sv"
#elif defined(GLQUAKE) && (defined(VKQUAKE) || defined(D3DQUAKE) || defined(SWQUAKE))
#	define ENGINE_RENDERER "m"
#elif defined(GLQUAKE)
#	define ENGINE_RENDERER "gl"
#elif defined(VKQUAKE)
#	define ENGINE_RENDERER "vk"
#elif defined(D3DQUAKE)
#	define ENGINE_RENDERER "d3d"
#else
#	define ENGINE_RENDERER "none"
#endif
#if defined(NOCOMPAT)
#	define ENGINE_CLIENT "-nc"
#elif defined(MINIMAL)
#	define ENGINE_CLIENT "-min"
#elif defined(CLIENTONLY)
#	define ENGINE_CLIENT "-cl"
#else
#	define ENGINE_CLIENT
#endif

#define THISARCH PLATFORM "_" ARCH_CPU_POSTFIX
#define THISENGINE THISARCH "-" DISTRIBUTION "-" ENGINE_RENDERER ENGINE_CLIENT

typedef struct package_s {
	char fullname[256];
	char *name;

	struct package_s *alternative;	//alternative (hidden) forms of this package.

	unsigned int trymirrors;
	char *mirror[8];
	char gamedir[16];
	enum fs_relative fsroot;
	char version[16];
	char *arch;
	char *qhash;

	char *description;
	char *license;
	char *author;
	char *previewimage;
	enum
	{
		EXTRACT_COPY,	//just copy the download over
		EXTRACT_XZ,		//give the download code a write filter so that it automatically decompresses on the fly
		EXTRACT_GZ,		//give the download code a write filter so that it automatically decompresses on the fly
		EXTRACT_ZIP		//extract stuff once it completes. kinda sucky.
	} extract;

	struct packagedep_s
	{
		struct packagedep_s *next;
		enum
		{
			DEP_CONFLICT,
			DEP_FILECONFLICT,	//don't install if this file already exists.
			DEP_REQUIRE,
			DEP_RECOMMEND,	//like depend, but uninstalling will not bubble.

			DEP_FILE
		} dtype;
		char name[1];
	} *deps;

	struct dl_download *download;

	int flags;
	int priority;
	struct package_s **link;
	struct package_s *next;
} package_t;

static qboolean loadedinstalled;
static package_t *availablepackages;
static int numpackages;

//FIXME: these are allocated for the life of the exe. changing games should purge the list.
static int numdownloadablelists = 0;
static struct
{
	char *url;
	char *prefix;
	char received;				//says if we got a response yet or not
	struct dl_download *curdl;	//the download context
} downloadablelist[32];
int downloadablessequence;	//bumped any time any package is purged

static void PM_FreePackage(package_t *p)
{
	struct packagedep_s *d;
	int i;

	if (p->link)
	{
		if (p->alternative)
		{	//replace it with its alternative package
			*p->link = p->alternative;
			p->alternative->alternative = p->alternative->next;
			if (p->alternative->alternative)
				p->alternative->alternative->link = &p->alternative->alternative;
			p->alternative->next = p->next;
			p->alternative->link = p->link;
		}
		else
		{	//just remove it from the list.
			*p->link = p->next;
			if (p->next)
				p->next->link = p->link;
		}
	}

	//free its data.
	while(p->deps)
	{
		d = p->deps;
		p->deps = d->next;
		Z_Free(d);
	}

	for (i = 0; i < countof(p->mirror); i++)
		Z_Free(p->mirror[i]);

	Z_Free(p->description);
	Z_Free(p->author);
	Z_Free(p->license);
	Z_Free(p->previewimage);
	Z_Free(p->qhash);
	Z_Free(p->arch);
	Z_Free(p);
}

//checks the status of each package
void PM_ValidatePackage(package_t *p)
{
	package_t *o;
	struct packagedep_s *dep;
	vfsfile_t *pf;
	p->flags &=~ (DPF_NATIVE|DPF_CACHED|DPF_CORRUPT);
	if (p->flags & DPF_INSTALLED)
	{
		for (dep = p->deps; dep; dep = dep->next)
		{
			char *n;
			if (dep->dtype != DEP_FILE)
				continue;
			if (*p->gamedir)
				n = va("%s/%s", p->gamedir, dep->name);
			else
				n = dep->name;
			pf = FS_OpenVFS(n, "rb", p->fsroot);
			if (pf)
			{
				VFS_CLOSE(pf);
				p->flags |= DPF_NATIVE;
			}
			else if (*p->gamedir && p->qhash)
			{
				char temp[MAX_OSPATH];
				if (FS_GenCachedPakName(n, p->qhash, temp, sizeof(temp)))
				{
					pf = FS_OpenVFS(temp, "rb", p->fsroot);
					if (pf)
					{
						VFS_CLOSE(pf);
						p->flags |= DPF_CACHED;
					}
				}
			}
			if (!(p->flags & (DPF_NATIVE|DPF_CACHED)))
				Con_Printf("WARNING: %s (%s) no longer exists\n", p->fullname, n);
		}
	}
	else
	{
		for (dep = p->deps; dep; dep = dep->next)
		{
			char *n;
			struct packagedep_s *odep;
			unsigned int fl = DPF_NATIVE;
			if (dep->dtype != DEP_FILE)
				continue;
			if (*p->gamedir)
				n = va("%s/%s", p->gamedir, dep->name);
			else
				n = dep->name;
			pf = FS_OpenVFS(n, "rb", p->fsroot);
			if (!pf && *p->gamedir && p->qhash)
			{
				char temp[MAX_OSPATH];
				if (FS_GenCachedPakName(n, p->qhash, temp, sizeof(temp)))
				{
					pf = FS_OpenVFS(temp, "rb", p->fsroot);
					fl = DPF_CACHED;
				}
				//fixme: skip any archive checks
			}

			if (pf)
			{
				for (o = availablepackages; o; o = o->next)
				{
					if (o == p)
						continue;
					if (o->flags & DPF_INSTALLED)
					{
						if (!strcmp(p->gamedir, o->gamedir) && p->fsroot == o->fsroot)
							if (strcmp(p->fullname, o->fullname) || strcmp(p->version, o->version))
							{
								for (odep = o->deps; odep; odep = odep->next)
								{
									if (!strcmp(dep->name, odep->name))
										break;
								}
								if (odep)
									break;
							}
					}
				}
				if (o && o->qhash && p->qhash && (o->flags & DPF_CACHED) && fl == DPF_CACHED)
					p->flags |= DPF_CACHED;
				else if (!o)
				{
					if (p->qhash)
					{
						char buf[8];
						searchpathfuncs_t *archive;

						if (!Q_strcasecmp(COM_FileExtension(n, buf, sizeof(buf)), "pak"))
							archive = FSPAK_LoadArchive(pf, n, NULL);
						else
						{
#ifdef AVAIL_ZLIB					//assume zip/pk3/pk4/apk/etc
							archive = FSZIP_LoadArchive(pf, n, NULL);
#else
							archive = NULL;
#endif
						}

						if (archive)
						{
							unsigned int fqhash;
							pf = NULL;
							fqhash = archive->GeneratePureCRC(archive, 0, 0);
							archive->ClosePath(archive);

							if (fqhash == (unsigned int)strtoul(p->qhash, NULL, 0))
							{
								p->flags |= fl;
								if (fl&DPF_NATIVE)
									p->flags |= DPF_MARKED|DPF_INSTALLED;
								break;
							}
							else
								pf = NULL;
						}
						else
							VFS_CLOSE(pf);
					}
					else
					{
						p->flags |= DPF_CORRUPT|fl;
						VFS_CLOSE(pf);
					}
					break;
				}
				VFS_CLOSE(pf);
			}
		}
	}
}

static qboolean PM_MergePackage(package_t *oldp, package_t *newp)
{
	//we don't track mirrors for previously-installed packages.
	//use the file list of the installed package, zips ignore the file list of the remote package but otherwise they must match to be mergeable
	//local installed copies of the package may lack some information, like mirrors.
	//the old package *might* be installed, the new won't be. this means we need to use the old's file list rather than the new
	if (!oldp->qhash || !strcmp(oldp->qhash?oldp->qhash:"", newp->qhash?newp->qhash:""))
	{
		unsigned int om, nm;
		struct packagedep_s *od, *nd;
		qboolean ignorefiles;
		for (om = 0; om < countof(oldp->mirror) && oldp->mirror[om]; om++)
			;
		for (nm = 0; nm < countof(newp->mirror) && newp->mirror[nm]; nm++)
			;
//		if (oldp->priority != newp->priority)
//			return false;

		ignorefiles = (oldp->extract==EXTRACT_ZIP);	//zips ignore the remote file list, its only important if its already installed (so just keep the old file list and its fine).
		if (oldp->extract != newp->extract)
		{	//if both have mirrors of different types then we have some sort of conflict
			if (ignorefiles || (om && nm))
				return false;
		}
		for (od = oldp->deps, nd = newp->deps; od && nd; )
		{
			//if its a zip then the 'remote' file list will be blank while the local list is not (we can just keep the local list).
			//if the file list DOES change, then bump the version.
			if (ignorefiles)
			{
				if (od->dtype == DEP_FILE)
				{
					od = od->next;
					continue;
				}
				if (nd->dtype == DEP_FILE)
				{
					nd = nd->next;
					continue;
				}
			}

			if (od->dtype != nd->dtype)
				return false;	//deps don't match
			if (strcmp(od->name, nd->name))
				return false;
			od = od->next;
			nd = nd->next;
		}

		//overwrite these. use the 'new' / remote values for each of them
		//the versions of the two packages will be the same, so the texts should be the same. still favour the new one so that things can be corrected serverside without needing people to redownload everything.
		if (newp->qhash){Z_Free(oldp->qhash); oldp->qhash = Z_StrDup(newp->qhash);}
		if (newp->description){Z_Free(oldp->description); oldp->description = Z_StrDup(newp->description);}
		if (newp->license){Z_Free(oldp->license); oldp->license = Z_StrDup(newp->license);}
		if (newp->author){Z_Free(oldp->author); oldp->author = Z_StrDup(newp->author);}
		if (newp->previewimage){Z_Free(oldp->previewimage); oldp->previewimage = Z_StrDup(newp->previewimage);}
		oldp->priority = newp->priority;

		if (nm)
		{	//copy over the mirrors
			oldp->extract = newp->extract;
			for (; nm --> 0 && om < countof(oldp->mirror); om++)
			{
				oldp->mirror[om] = newp->mirror[nm];
				newp->mirror[nm] = NULL;
			}
		}
		oldp->flags &= ~DPF_FORGETONUNINSTALL | (newp->flags & DPF_FORGETONUNINSTALL);

		PM_FreePackage(newp);
		return true;
	}
	return false;
}

static void PM_InsertPackage(package_t *p)
{
	package_t **link;
	for (link = &availablepackages; *link; link = &(*link)->next)
	{
		package_t *prev = *link;
		int v = strcmp(prev->fullname, p->fullname);
		if (v > 0)
			break;	//insert before this one
		else if (v == 0)
		{	//name matches.
			//if (!strcmp(p->fullname),prev->fullname)
			if (!strcmp(p->version, prev->version))
			if (!strcmp(p->gamedir, prev->gamedir))
			if (!strcmp(p->arch?p->arch:"", prev->arch?prev->arch:""))
			{ /*package matches, merge them somehow, don't add*/
				package_t *a;
				if (PM_MergePackage(prev, p))
					return;
				for (a = p->alternative; a; a = a->next)
				{
					if (PM_MergePackage(a, p))
						return;
				}
				p->next = prev->alternative;
				prev->alternative = p;
				p->link = &prev->alternative;
				return;
			}

			//something major differs, display both independantly.
			p->flags |= DPF_DISPLAYVERSION;
			prev->flags |= DPF_DISPLAYVERSION;
		}
	}
	p->next = *link;
	p->link = link;
	*link = p;
	PM_ValidatePackage(p);
	numpackages++;
}


static qboolean PM_CheckFile(const char *filename, enum fs_relative base)
{
	vfsfile_t *f = FS_OpenVFS(filename, "rb", base);
	if (f)
	{
		VFS_CLOSE(f);
		return true;
	}
	return false;
}
static void PM_AddDep(package_t *p, int deptype, const char *depname)
{
	struct packagedep_s *nd, **link;

	//no dupes.
	for (link = &p->deps; (nd=*link) ; link = &nd->next)
	{
		if (nd->dtype == deptype && !strcmp(nd->name, depname))
			return;
	}

	//add it on the end, preserving order.
	nd = Z_Malloc(sizeof(*nd) + strlen(depname));
	nd->dtype = deptype;
	strcpy(nd->name, depname);
	nd->next = *link;
	*link = nd;
}

static void PM_AddSubList(char *url, const char *prefix)
{
	int i;

	for (i = 0; i < numdownloadablelists; i++)
	{
		if (!strcmp(downloadablelist[i].url, url))
			break;
	}
	if (i == numdownloadablelists && i < countof(downloadablelist))
	{
		downloadablelist[i].url = BZ_Malloc(strlen(url)+1);
		strcpy(downloadablelist[i].url, url);

		downloadablelist[i].prefix = BZ_Malloc(strlen(prefix)+1);
		strcpy(downloadablelist[i].prefix, prefix);

		numdownloadablelists++;
	}
}

static void PM_ParsePackageList(vfsfile_t *f, int parseflags, const char *url, const char *prefix)
{
	char line[65536];
	package_t *p;
	struct packagedep_s *dep;
	char *sl;

	int version;
	char defaultgamedir[64];
	char mirror[countof(p->mirror)][MAX_OSPATH];
	int nummirrors = 0;
	int argc;

	if (!f)
		return;

	Q_strncpyz(defaultgamedir, FS_GetGamedir(false), sizeof(defaultgamedir));

	if (url)
	{
		Q_strncpyz(mirror[nummirrors], url, sizeof(mirror[nummirrors]));
		sl = COM_SkipPath(mirror[nummirrors]);
		*sl = 0;
		nummirrors++;
	}

	do
	{
		if (!VFS_GETS(f, line, sizeof(line)-1))
			break;
		while((sl=strchr(line, '\n')))
			*sl = '\0';
		while((sl=strchr(line, '\r')))
			*sl = '\0';
		Cmd_TokenizeString (line, false, false);
	} while (!Cmd_Argc());

	if (strcmp(Cmd_Argv(0), "version"))
		return;	//it's not the right format.

	version = atoi(Cmd_Argv(1));
	if (version != 0 && version != 1 && version != 2)
	{
		Con_Printf("Packagelist is of a future or incompatible version\n");
		return;	//it's not the right version.
	}

	while(1)
	{
		if (!VFS_GETS(f, line, sizeof(line)-1))
			break;
		while((sl=strchr(line, '\n')))
			*sl = '\0';
		while((sl=strchr(line, '\r')))
			*sl = '\0';
		Cmd_TokenizeString (line, false, false);
		argc = Cmd_Argc();
		if (argc)
		{
			if (!strcmp(Cmd_Argv(0), "sublist"))
			{
				char *subprefix;
				if (*prefix)
					subprefix = va("%s/%s", prefix, Cmd_Argv(2));
				else
					subprefix = Cmd_Argv(2);
				PM_AddSubList(Cmd_Argv(1), subprefix);
				continue;
			}
			if (!strcmp(Cmd_Argv(0), "set"))
			{
				if (!strcmp(Cmd_Argv(1), "gamedir"))
				{
					if (argc == 2)
						Q_strncpyz(defaultgamedir, FS_GetGamedir(false), sizeof(defaultgamedir));
					else
						Q_strncpyz(defaultgamedir, Cmd_Argv(2), sizeof(defaultgamedir));
				}
				else if (!strcmp(Cmd_Argv(1), "mirrors"))
				{
					nummirrors = 0;
					while (nummirrors < countof(mirror) && 2+nummirrors < argc)
					{
						Q_strncpyz(mirror[nummirrors], Cmd_Argv(2+nummirrors), sizeof(mirror[nummirrors]));
						if (!*mirror[nummirrors])
							break;
						nummirrors++;
					}
				}
				else
				{
					//erk
				}
				continue;
			}
			if (version > 1)
			{
				char *fullname = Cmd_Argv(0);
				char *file = NULL;
				char *url = NULL;
				char *gamedir = NULL;
				char *ver = NULL;
				char *arch = NULL;
				char *qhash = NULL;
				char *description = NULL;
				char *license = NULL;
				char *author = NULL;
				char *previewimage = NULL;
				int extract = EXTRACT_COPY;
				int priority = PM_DEFAULTPRIORITY;
				unsigned int flags = parseflags;
				int i;

				if (version > 2)
					flags &= DPF_INSTALLED;

				p = Z_Malloc(sizeof(*p));
				for (i = 1; i < argc; i++)
				{
					char *arg = Cmd_Argv(i);
					if (!strncmp(arg, "url=", 4))
						url = arg+4;
					else if (!strncmp(arg, "gamedir=", 8))
						gamedir = arg+8;
					else if (!strncmp(arg, "ver=", 4))
						ver = arg+4;
					else if (!strncmp(arg, "v=", 2))
						ver = arg+2;
					else if (!strncmp(arg, "arch=", 5))
						arch = arg+5;
					else if (!strncmp(arg, "priority=", 9))
						priority = atoi(arg+9);
					else if (!strncmp(arg, "qhash=", 6))
						qhash = arg+6;
					else if (!strncmp(arg, "desc=", 5))
						description = arg+5;
					else if (!strncmp(arg, "license=", 8))
						license = arg+8;
					else if (!strncmp(arg, "author=", 7))
						author = arg+7;
					else if (!strncmp(arg, "preview=", 8))
						previewimage = arg+8;
					else if (!strncmp(arg, "file=", 5))
					{
						if (!file)
							file = arg+5;
						PM_AddDep(p, DEP_FILE, arg+5);
					}
					else if (!strncmp(arg, "extract=", 8))
					{
						if (!strcmp(arg+8, "xz"))
							extract = EXTRACT_XZ;
						else if (!strcmp(arg+8, "gz"))
							extract = EXTRACT_GZ;
						else if (!strcmp(arg+8, "zip"))
							extract = EXTRACT_ZIP;
						else
							Con_Printf("Unknown decompression method: %s\n", arg+8);
					}
					else if (!strncmp(arg, "depend=", 7))
						PM_AddDep(p, DEP_REQUIRE, arg+7);
					else if (!strncmp(arg, "conflict=", 9))
						PM_AddDep(p, DEP_CONFLICT, arg+9);
					else if (!strncmp(arg, "fileconflict=", 13))
						PM_AddDep(p, DEP_FILECONFLICT, arg+13);
					else if (!strncmp(arg, "recommend=", 10))
						PM_AddDep(p, DEP_RECOMMEND, arg+10);
					else if (!strncmp(arg, "stale=", 6) && version==2)
						flags &= ~DPF_INSTALLED;
					else if (!strncmp(arg, "installed=", 6) && version>2)
						flags |= parseflags & DPF_INSTALLED;
					else
					{
						Con_DPrintf("Unknown package property\n");
					}
				}

				if (*prefix)
					Q_snprintfz(p->fullname, sizeof(p->fullname), "%s/%s", prefix, fullname);
				else
					Q_snprintfz(p->fullname, sizeof(p->fullname), "%s", fullname);
				p->name = COM_SkipPath(p->fullname);

				if (!gamedir)
					gamedir = defaultgamedir;

				Q_strncpyz(p->version, ver?ver:"", sizeof(p->version));

				Q_snprintfz(p->gamedir, sizeof(p->gamedir), "%s", gamedir);
				p->fsroot = FS_ROOT;
				p->extract = extract;
				p->priority = priority;
				p->flags = flags;

				p->arch = arch?Z_StrDup(arch):NULL;
				p->qhash = qhash?Z_StrDup(qhash):NULL;
				p->description = description?Z_StrDup(description):NULL;
				p->license = license?Z_StrDup(license):NULL;
				p->author = author?Z_StrDup(author):NULL;
				p->previewimage = previewimage?Z_StrDup(previewimage):NULL;

				if (url && (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)))
					p->mirror[0] = Z_StrDup(url);
				else
				{
					int m;
					char *ext = "";
					if (!url)
					{
						if (extract == EXTRACT_XZ)
							ext = ".xz";
						else if (extract == EXTRACT_GZ)
							ext = ".gz";
						else if (extract == EXTRACT_ZIP)
							ext = ".zip";
						url = file;
					}
					if (url)
					{
						for (m = 0; m < nummirrors; m++)
							p->mirror[m] = Z_StrDup(va("%s%s%s", mirror[m], url, ext));
					}
				}
			}
			else
			{
				if (argc > 5 || argc < 3)
				{
					Con_Printf("Package list is bad - %s\n", line);
					continue;	//but try the next line away
				}

				p = Z_Malloc(sizeof(*p));

				if (*prefix)
					Q_strncpyz(p->fullname, va("%s/%s", prefix, Cmd_Argv(0)), sizeof(p->fullname));
				else
					Q_strncpyz(p->fullname, Cmd_Argv(0), sizeof(p->fullname));
				p->name = p->fullname;
				while((sl = strchr(p->name, '/')))
					p->name = sl+1;

				p->priority = PM_DEFAULTPRIORITY;
				p->flags = parseflags;

				p->mirror[0] = Z_StrDup(Cmd_Argv(1));
				PM_AddDep(p, DEP_FILE, Cmd_Argv(2));
				Q_strncpyz(p->version, Cmd_Argv(3), sizeof(p->version));
				Q_strncpyz(p->gamedir, Cmd_Argv(4), sizeof(p->gamedir));
				if (!strcmp(p->gamedir, "../"))
				{
					p->fsroot = FS_ROOT;
					*p->gamedir = 0;
				}
				else
				{
					if (!*p->gamedir)
					{
						strcpy(p->gamedir, FS_GetGamedir(false));
		//				p->fsroot = FS_GAMEONLY;
					}
					p->fsroot = FS_ROOT;
				}
			}

			if (p->arch)
			{
				if (!Q_strcasecmp(p->arch, THISENGINE))
				{
					if (Sys_GetAutoUpdateSetting() == UPD_UNSUPPORTED)
						p->flags |= DPF_HIDDEN;
					else
						p->flags |= DPF_ENGINE;
				}
				else if (!Q_strcasecmp(p->arch, THISARCH))
					;
				else
					p->flags |= DPF_HIDDEN;	//other engine builds or other cpus are all hidden
			}
			for (dep = p->deps; dep; dep = dep->next)
			{
				if (dep->dtype == DEP_FILECONFLICT)
				{
					const char *n;
					if (*p->gamedir)
						n = va("%s/%s", p->gamedir, dep->name);
					else
						n = dep->name;
					if (PM_CheckFile(n, p->fsroot))
						p->flags |= DPF_HIDDEN;
				}
			}
			if (p->flags & DPF_INSTALLED)
				p->flags |= DPF_MARKED;

			PM_InsertPackage(p);
		}
	}
}

void PM_LoadPackages(searchpath_t **oldpaths, const char *parent_pure, const char *parent_logical, searchpath_t *search, unsigned int loadstuff, int minpri, int maxpri)
{
	package_t *p;
	struct packagedep_s *d;
	char temp[MAX_OSPATH];
	int pri;

	//figure out what we've previously installed.
	if (!loadedinstalled)
	{
		vfsfile_t *f = FS_OpenVFS(INSTALLEDFILES, "rb", FS_ROOT);
		loadedinstalled = true;
		if (f)
		{
			PM_ParsePackageList(f, DPF_FORGETONUNINSTALL|DPF_INSTALLED, NULL, "");
			VFS_CLOSE(f);
		}
	}

	do
	{
		//find the lowest used priority above the previous
		pri = maxpri;
		for (p = availablepackages; p; p = p->next)
		{
			if ((p->flags & DPF_INSTALLED) && p->qhash && p->priority>=minpri&&p->priority<pri && !Q_strcasecmp(parent_pure, p->gamedir))
				pri = p->priority;
		}
		minpri = pri+1;

		for (p = availablepackages; p; p = p->next)
		{
			if ((p->flags & DPF_INSTALLED) && p->qhash && p->priority==pri && !Q_strcasecmp(parent_pure, p->gamedir))
			{
				for (d = p->deps; d; d = d->next)
				{
					if (d->dtype == DEP_FILE)
					{
						Q_snprintfz(temp, sizeof(temp), "%s/%s", p->gamedir, d->name);
						FS_AddHashedPackage(oldpaths, parent_pure, parent_logical, search, loadstuff, temp, p->qhash, NULL);
					}
				}
			}
		}
	} while (pri < maxpri);
}

void PM_Shutdown(void)
{
	//free everything...
	loadedinstalled = false;
	fs_downloads_url.modified = false;

	downloadablessequence++;

	while(numdownloadablelists > 0)
	{
		numdownloadablelists--;

		if (downloadablelist[numdownloadablelists].curdl)
		{
			DL_Close(downloadablelist[numdownloadablelists].curdl);
			downloadablelist[numdownloadablelists].curdl = NULL;
		}
		downloadablelist[numdownloadablelists].received = 0;
		Z_Free(downloadablelist[numdownloadablelists].url);
		downloadablelist[numdownloadablelists].url = NULL;
		Z_Free(downloadablelist[numdownloadablelists].prefix);
		downloadablelist[numdownloadablelists].prefix = NULL;
	}

	while (availablepackages)
		PM_FreePackage(availablepackages);
}

#ifndef SERVERONLY
qboolean doautoupdate;

static void PM_PreparePackageList(void)
{
	//figure out what we've previously installed.
	if (!loadedinstalled)
	{
		vfsfile_t *f = FS_OpenVFS(INSTALLEDFILES, "rb", FS_ROOT);
		loadedinstalled = true;
		if (f)
		{
			PM_ParsePackageList(f, DPF_FORGETONUNINSTALL|DPF_INSTALLED, NULL, "");
			VFS_CLOSE(f);
		}
	}
}



//finds the newest version
static package_t *PM_FindPackage(char *packagename)
{
	package_t *p, *r = NULL;

	for (p = availablepackages; p; p = p->next)
	{
		if (!strcmp(p->fullname, packagename))
		{
			if (!r || strcmp(r->version, p->version)>0)
				r = p;
		}
	}
	if (r)
		return r;

	for (p = availablepackages; p; p = p->next)
	{
		if (!strcmp(p->name, packagename))
		{
			if (!r || strcmp(r->version, p->version)>0)
				r = p;
		}
	}
	return r;
}
//returns the marked version of a package, if any.
static package_t *PM_MarkedPackage(char *packagename)
{
	package_t *p;
	for (p = availablepackages; p; p = p->next)
	{
		if (p->flags & DPF_MARKED)
			if (!strcmp(p->name, packagename) || !strcmp(p->fullname, packagename))
				return p;
	}
	return NULL;
}

//just flags, doesn't delete
static void PM_UnmarkPackage(package_t *package)
{
	package_t *o;
	struct packagedep_s *dep;

	if (!(package->flags & DPF_MARKED))
		return;	//looks like its already deselected.
	package->flags &= ~(DPF_MARKED);

	//Is this safe?
	package->trymirrors = 0;	//if its enqueued, cancel that quickly...
	if (package->download)
	{					//if its currently downloading, cancel it.
		DL_Close(package->download);
		package->download = NULL;
	}

	//remove stuff that depends on us
	for (o = availablepackages; o; o = o->next)
	{
		for (dep = o->deps; dep; dep = dep->next)
			if (dep->dtype == DEP_REQUIRE)
				if (!strcmp(dep->name, package->name) || !strcmp(dep->name, package->fullname))
					PM_UnmarkPackage(o);
	}
}

//just flags, doesn't install
static void PM_MarkPackage(package_t *package)
{
	package_t *o;
	struct packagedep_s *dep, *dep2;
	qboolean replacing = false;

	if (package->flags & DPF_MARKED)
		return;	//looks like its already picked.

	//any file-conflicts prevent the package from being installable.
	//this is mostly for pak1.pak
	for (dep = package->deps; dep; dep = dep->next)
	{
		if (dep->dtype == DEP_FILECONFLICT)
		{
			const char *n;
			if (*package->gamedir)
				n = va("%s/%s", package->gamedir, dep->name);
			else
				n = dep->name;
			if (PM_CheckFile(n, package->fsroot))
				return;
		}
	}

	package->flags |= DPF_MARKED;

	//first check to see if we're replacing a different version of the same package
	for (o = availablepackages; o; o = o->next)
	{
		if (o == package)
			continue;

		if (o->flags & DPF_MARKED)
		{
			if (!strcmp(o->fullname, package->fullname))
			{	//replaces this package
				o->flags &= ~DPF_MARKED;
				replacing = true;
			}
			else
			{	//two packages with the same filename are always mutually incompatible, but with totally separate dependancies etc.
				qboolean remove = false;
				for (dep = package->deps; dep; dep = dep->next)
				{
					if (dep->dtype == DEP_FILE)
					for (dep2 = o->deps; dep2; dep2 = dep2->next)
					{
						if (dep2->dtype == DEP_FILE)
						if (!strcmp(dep->name, dep2->name))
						{
							PM_UnmarkPackage(o);
							remove = true;
							break;
						}
					}
					if (remove)
						break;
				}
				//fixme: zip content conflicts
			}
		}
	}

	//if we are replacing an existing one, then dependancies are already settled (only because we don't do version deps)
	if (replacing)
		return;

	//satisfy our dependancies.
	for (dep = package->deps; dep; dep = dep->next)
	{
		if (dep->dtype == DEP_REQUIRE || dep->dtype == DEP_RECOMMEND)
		{
			package_t *d = PM_MarkedPackage(dep->name);
			if (!d)
			{
				d = PM_FindPackage(dep->name);
				if (d)
					PM_MarkPackage(d);
				else
					Con_DPrintf("Couldn't find dependancy \"%s\"\n", dep->name);
			}
		}
		if (dep->dtype == DEP_CONFLICT)
		{
			for (;;)
			{
				package_t *d = PM_MarkedPackage(dep->name);
				if (!d)
					break;
				PM_UnmarkPackage(d);
			}
		}
	}

	//remove any packages that conflict with us.
	for (o = availablepackages; o; o = o->next)
	{
		for (dep = o->deps; dep; dep = dep->next)
			if (dep->dtype == DEP_CONFLICT)
				if (!strcmp(dep->name, package->fullname) || !strcmp(dep->name, package->name))
					PM_UnmarkPackage(o);
	}
}

//just flag stuff as needing updating
static unsigned int PM_MarkUpdates (void)
{
	unsigned int changecount = 0;
	package_t *p, *o, *b, *e = NULL;
	for (p = availablepackages; p; p = p->next)
	{
		if ((p->flags & DPF_ENGINE) && !(p->flags & DPF_HIDDEN))
		{
			if ((p->flags & DPF_MARKED) || !e || strcmp(e->version, p->version) < 0)
				e = p;
		}
		if (p->flags & DPF_MARKED)
		{
			b = NULL;
			for (o = availablepackages; o; o = o->next)
			{
				if (p == o || (o->flags & DPF_HIDDEN))
					continue;
				if (!strcmp(o->fullname, p->fullname) && !strcmp(o->arch?o->arch:"", p->arch?p->arch:"") && strcmp(o->version, p->version) > 0)
				{
					if (!b || strcmp(b->version, o->version) < 0)
						b = o;
				}
			}

			if (b)
			{
				changecount++;
				PM_MarkPackage(b);
				PM_UnmarkPackage(p);
			}
		}
	}
	if (e && !(e->flags & DPF_MARKED))
	{
		if (Sys_GetAutoUpdateSetting() >= UPD_STABLE)
		{
			changecount++;
			PM_MarkPackage(e);
		}
	}

	return changecount;
}

static void PM_ListDownloaded(struct dl_download *dl)
{
	int i;
	vfsfile_t *f;
	f = dl->file;
	dl->file = NULL;

	i = dl->user_num;

	if (dl != downloadablelist[i].curdl)
	{
		//this request looks stale.
		VFS_CLOSE(f);
		return;
	}
	downloadablelist[i].curdl = NULL;

	if (f)
	{
		downloadablelist[i].received = 1;
		PM_ParsePackageList(f, 0, dl->url, downloadablelist[i].prefix);
		VFS_CLOSE(f);
	}
	else
		downloadablelist[i].received = -1;

	if (!doautoupdate)
		return;	//don't spam this.
	for (i = 0; i < numdownloadablelists; i++)
	{
		if (!downloadablelist[i].received)
			break;
	}
	if (i == numdownloadablelists)
	{
		doautoupdate = true;
		if (PM_MarkUpdates())
		{
			if (Key_Dest_Has(kdm_emenu))
			{
				Key_Dest_Remove(kdm_emenu);
				m_state = m_none;
			}
#ifdef MENU_DAT
			if (Key_Dest_Has(kdm_gmenu))
				MP_Toggle(0);
#endif
			Cmd_ExecuteString("menu_download\n", RESTRICT_LOCAL);
		}
	}
}
//retry 1==
static void PM_UpdatePackageList(qboolean autoupdate, int retry)
{
	unsigned int i;

	if (retry>1 || fs_downloads_url.modified)
		PM_Shutdown();

	PM_PreparePackageList();

	//make sure our sources are okay.
	if (*fs_downloads_url.string)
		PM_AddSubList(fs_downloads_url.string, "");

	doautoupdate |= autoupdate;

	//kick off the initial tier of downloads.
	for (i = 0; i < numdownloadablelists; i++)
	{
		if (downloadablelist[i].received)
			continue;
		autoupdate = false;
		if (downloadablelist[i].curdl)
			continue;

		downloadablelist[i].curdl = HTTP_CL_Get(va("%s"DOWNLOADABLESARGS, downloadablelist[i].url), NULL, PM_ListDownloaded);
		if (downloadablelist[i].curdl)
		{
			downloadablelist[i].curdl->user_num = i;

			downloadablelist[i].curdl->file = VFSPIPE_Open();
			downloadablelist[i].curdl->isquery = true;
			DL_CreateThread(downloadablelist[i].curdl, NULL, NULL);
		}
		else
		{
			Con_Printf("Could not contact server - %s\n", downloadablelist[i].url);
			downloadablelist[i].received = -1;
		}
	}

	if (autoupdate)
	{
		doautoupdate = 0;
		if (PM_MarkUpdates())
		{
			Cbuf_AddText("menu_download\n", RESTRICT_LOCAL);
		}
	}
}



typedef struct {
	menucustom_t *list;
	char intermediatefilename[MAX_QPATH];
	char pathprefix[MAX_QPATH];
	int downloadablessequence;
	qboolean populated;
} dlmenu_t;

static int autoupdatesetting = UPD_UNSUPPORTED;

static void COM_QuotedConcat(const char *cat, char *buf, size_t bufsize)
{
	const unsigned char *gah;
	for (gah = (const unsigned char*)cat; *gah; gah++)
	{
		if (*gah <= ' ' || *gah == '$' || *gah == '\"' || *gah == '\n' || *gah == '\r')
			break;
	}
	if (*gah || *cat == '\\' ||
		strstr(cat, "//") || strstr(cat, "/*"))
	{	//contains some dodgy stuff.
		size_t curlen = strlen(buf);
		buf += curlen;
		bufsize -= curlen;
		COM_QuotedString(cat, buf, bufsize, false);
	}
	else
	{	//okay, no need for quotes.
		Q_strncatz(buf, cat, bufsize);
	}
}
static void PM_WriteInstalledPackages(void)
{
	char *s;
	package_t *p, *e = NULL;
	struct packagedep_s *dep, *ef = NULL;
	vfsfile_t *f = FS_OpenVFS(INSTALLEDFILES, "wb", FS_ROOT);
	if (!f)
	{
		Con_Printf("menu_download: Can't update installed list\n");
		return;
	}

	s = "version 2\n";
	VFS_WRITE(f, s, strlen(s));
	for (p = availablepackages; p ; p=p->next)
	{
		if (p->flags & (DPF_CACHED|DPF_INSTALLED))
		{
			char buf[8192];
			buf[0] = 0;
			COM_QuotedString(p->fullname, buf, sizeof(buf), false);
			if (p->flags & DPF_INSTALLED)
			{	//v3+
//				Q_strncatz(buf, " ", sizeof(buf));
//				COM_QuotedConcat(va("installed=1"), buf, sizeof(buf));
			}
			else
			{	//v2
				Q_strncatz(buf, " ", sizeof(buf));
				COM_QuotedConcat(va("stale=1"), buf, sizeof(buf));
			}
			if (*p->version)
			{
				Q_strncatz(buf, " ", sizeof(buf));
				COM_QuotedConcat(va("ver=%s", p->version), buf, sizeof(buf));
			}
			//if (*p->gamedir)
			{
				Q_strncatz(buf, " ", sizeof(buf));
				COM_QuotedConcat(va("gamedir=%s", p->gamedir), buf, sizeof(buf));
			}
			if (p->qhash)
			{
				Q_strncatz(buf, " ", sizeof(buf));
				COM_QuotedConcat(va("qhash=%s", p->qhash), buf, sizeof(buf));
			}
			if (p->priority!=PM_DEFAULTPRIORITY)
			{
				Q_strncatz(buf, " ", sizeof(buf));
				COM_QuotedConcat(va("priority=%i", p->priority), buf, sizeof(buf));
			}
			if (p->arch)
			{
				Q_strncatz(buf, " ", sizeof(buf));
				COM_QuotedConcat(va("arch=%s", p->arch), buf, sizeof(buf));
			}

			if (p->description)
			{
				Q_strncatz(buf, " ", sizeof(buf));
				COM_QuotedConcat(va("description=%s", p->description), buf, sizeof(buf));
			}
			if (p->license)
			{
				Q_strncatz(buf, " ", sizeof(buf));
				COM_QuotedConcat(va("license=%s", p->license), buf, sizeof(buf));
			}
			if (p->author)
			{
				Q_strncatz(buf, " ", sizeof(buf));
				COM_QuotedConcat(va("author=%s", p->author), buf, sizeof(buf));
			}
			if (p->previewimage)
			{
				Q_strncatz(buf, " ", sizeof(buf));
				COM_QuotedConcat(va("preview=%s", p->previewimage), buf, sizeof(buf));
			}

			for (dep = p->deps; dep; dep = dep->next)
			{
				if (dep->dtype == DEP_FILE)
				{
					Q_strncatz(buf, " ", sizeof(buf));
					COM_QuotedConcat(va("file=%s", dep->name), buf, sizeof(buf));
					if ((p->flags & DPF_ENGINE) && (!e || strcmp(e->version, p->version) < 0))
					{
						e = p;
						ef = dep;
					}
				}
				else if (dep->dtype == DEP_REQUIRE)
				{
					Q_strncatz(buf, " ", sizeof(buf));
					COM_QuotedConcat(va("depend=%s", dep->name), buf, sizeof(buf));
				}
				else if (dep->dtype == DEP_CONFLICT)
				{
					Q_strncatz(buf, " ", sizeof(buf));
					COM_QuotedConcat(va("conflict=%s", dep->name), buf, sizeof(buf));
				}
				else if (dep->dtype == DEP_FILECONFLICT)
				{
					Q_strncatz(buf, " ", sizeof(buf));
					COM_QuotedConcat(va("fileconflict=%s", dep->name), buf, sizeof(buf));
				}
				else if (dep->dtype == DEP_RECOMMEND)
				{
					Q_strncatz(buf, " ", sizeof(buf));
					COM_QuotedConcat(va("recommend=%s", dep->name), buf, sizeof(buf));
				}
			}

			buf[sizeof(buf)-2] = 0;	//just in case.
			Q_strncatz(buf, "\n", sizeof(buf));
			VFS_WRITE(f, buf, strlen(buf));
		}
	}

	VFS_CLOSE(f);

	if (ef)
	{
		char native[MAX_OSPATH];
		FS_NativePath(ef->name, e->fsroot, native, sizeof(native));
		Sys_SetUpdatedBinary(native);
	}
}

static void MD_Draw (int x, int y, struct menucustom_s *c, struct menu_s *m)
{
	package_t *p;
	char *n;
	if (c->dint != downloadablessequence)
		return;	//probably stale
	p = c->dptr;
	if (p)
	{
		if (p->alternative && (p->flags & DPF_HIDDEN))
			p = p->alternative;

		if (p->download)
			Draw_FunString (x+4, y, va("%i", (int)p->download->qdownload.percent));
		else if (p->trymirrors)
			Draw_FunString (x+4, y, "PND");
		else 
		{
			switch((p->flags & (DPF_INSTALLED | DPF_MARKED)))
			{
			case 0:
				if (p->flags & DPF_PURGE)
					Draw_FunString (x, y, "DEL");	//purge
				else if (p->flags & DPF_HIDDEN)
					Draw_FunString (x+4, y, "---");
				else if (p->flags & DPF_CORRUPT)
					Draw_FunString (x, y, "!!!");
				else
				{
					Draw_FunString (x+4, y, "^Ue080^Ue082");
					Draw_FunString (x+8, y, "^Ue081");
				}
				break;
			case DPF_INSTALLED:
				if (p->flags & DPF_PURGE || !(p->qhash && (p->flags & DPF_CACHED)))
					Draw_FunString (x, y, "DEL");
				else
					Draw_FunString (x, y, "REM");
				break;
			case DPF_MARKED:
				if (p->flags & DPF_PURGE)
					Draw_FunString (x, y, "GET");
				else if (p->flags & (DPF_CACHED|DPF_NATIVE))
					Draw_FunString (x, y, "USE");
				else
					Draw_FunString (x, y, "GET");
				break;
			case DPF_INSTALLED | DPF_MARKED:
				if (p->flags & DPF_PURGE)
					Draw_FunString (x, y, "GET");	//purge and reinstall.
				else if (p->flags & DPF_CORRUPT)
					Draw_FunString (x, y, "?""?""?");
				else
				{
					Draw_FunString (x+4, y, "^Ue080^Ue082");
					Draw_FunString (x+8, y, "^Ue083");
				}
				break;
			}
		}

		n = p->name;
		if (p->flags & DPF_DISPLAYVERSION)
			n = va("%s (%s)", n, *p->version?p->version:"unversioned");

		if (&m->selecteditem->common == &c->common)
			Draw_AltFunString (x+48, y, n);
		else
			Draw_FunString(x+48, y, n);
	}
}

static qboolean MD_Key (struct menucustom_s *c, struct menu_s *m, int key, unsigned int unicode)
{
	package_t *p, *p2;
	struct packagedep_s *dep, *dep2;
	if (c->dint != downloadablessequence)
		return false;	//probably stale
	p = c->dptr;
	if (key == K_ENTER || key == K_KP_ENTER || key == K_MOUSE1)
	{
		if (p->alternative && (p->flags & DPF_HIDDEN))
			p = p->alternative;

		if (p->flags & DPF_INSTALLED)
		{
			switch (p->flags & (DPF_PURGE|DPF_MARKED))
			{
			case DPF_MARKED:
				PM_UnmarkPackage(p);	//deactivate it
				break;
			case 0:
				p->flags |= DPF_PURGE;	//purge
				if (p->flags & (DPF_CACHED | DPF_CORRUPT))
					break;
				//fall through
			case DPF_PURGE:
				PM_MarkPackage(p);		//reinstall
//				if (!(p->flags & DPF_HIDDEN) && !(p->flags & DPF_CACHED))
//					break;
				//fall through
			case DPF_MARKED|DPF_PURGE:
				p->flags &= ~DPF_PURGE;	//back to no-change
				break;
			}
		}
		else
		{
			switch (p->flags & (DPF_PURGE|DPF_MARKED))
			{
			case 0:
				PM_MarkPackage(p);
				//now: try to install
				break;
			case DPF_MARKED:
				p->flags |= DPF_PURGE;
				//now: re-get despite already having it.
				if (p->flags & (DPF_CACHED | DPF_CORRUPT))
					break;	//only makes sense if we already have a cached copy that we're not going to use.
				//fallthrough
			case DPF_MARKED|DPF_PURGE:
				PM_UnmarkPackage(p);
				//now: delete
				if (p->flags & (DPF_CACHED | DPF_CORRUPT))
					break;	//only makes sense if we have a cached/corrupt copy of it already
				//fallthrough
			case DPF_PURGE:
				p->flags &= ~DPF_PURGE;
				//now: no change
				break;
			}
		}

		if (p->flags&DPF_MARKED)
		{
			//any other packages that conflict should be flagged for uninstall now that this one will replace it.
			for (p2 = availablepackages; p2; p2 = p2->next)
			{
				if (p == p2)
					continue;
				for (dep = p->deps; dep; dep = dep->next)
				{
					if (dep->dtype != DEP_FILE)
						continue;
					for (dep2 = p2->deps; dep2; dep2 = dep2->next)
					{
						if (dep2->dtype != DEP_FILE)
							continue;
						if (!strcmp(dep->name, dep2->name))
						{
							PM_UnmarkPackage(p2);
							break;
						}
					}
				}
			}
		}
		else
			p->trymirrors = 0;
		return true;
	}

	return false;
}

static void MD_AutoUpdate_Draw (int x, int y, struct menucustom_s *c, struct menu_s *m)
{
	char *settings[] = 
	{
		"Unsupported",
		"Revert",
		"Off",
		"Stable Updates",
		"Unsable Updates"
	};
	char *text;
	int setting = Sys_GetAutoUpdateSetting();
	if (setting == UPD_UNSUPPORTED)
		text = va("Auto Update: %s", settings[autoupdatesetting+1]);
	else if (autoupdatesetting == UPD_UNSUPPORTED)
		text = va("Auto Update: %s", settings[setting+1]);
	else
		text = va("Auto Update: %s (unsaved)", settings[autoupdatesetting+1]);
	if (&m->selecteditem->common == &c->common)
		Draw_AltFunString (x+4, y, text);
	else
		Draw_FunString (x+4, y, text);
}
static qboolean MD_AutoUpdate_Key (struct menucustom_s *c, struct menu_s *m, int key, unsigned int unicode)
{
	if (key == K_ENTER || key == K_KP_ENTER || key == K_MOUSE1)
	{
		if (autoupdatesetting == UPD_UNSUPPORTED)
			autoupdatesetting = min(0, Sys_GetAutoUpdateSetting());
		autoupdatesetting+=1;
		if (autoupdatesetting > UPD_TESTING)
			autoupdatesetting = (Sys_GetAutoUpdateSetting() == UPD_UNSUPPORTED)?1:0;
		PM_UpdatePackageList(true, 2);
	}
	return false;
}

qboolean MD_PopMenu (union menuoption_s *mo,struct menu_s *m,int key)
{
	if (key == K_ENTER || key == K_KP_ENTER || key == K_MOUSE1)
	{
		M_RemoveMenu(m);
		return true;
	}
	return false;
}

vfsfile_t *FS_XZ_DecompressWriteFilter(vfsfile_t *infile);
vfsfile_t *FS_GZ_DecompressWriteFilter(vfsfile_t *outfile, qboolean autoclosefile);

static char *MD_GetTempName(package_t *p)
{
	struct packagedep_s *dep, *fdep;
	char *destname, *t, *ts;
	//always favour the file so that we can rename safely without needing a copy.
	for (dep = p->deps, fdep = NULL; dep; dep = dep->next)
	{
		if (dep->dtype != DEP_FILE)
			continue;
		if (fdep)
		{
			fdep = NULL;
			break;
		}
		fdep = dep;
	}
	if (fdep)
	{
		if (*p->gamedir)
			destname = va("%s/%s.tmp", p->gamedir, fdep->name);
		else
			destname = va("%s.tmp", fdep->name);
		return Z_StrDup(destname);
	}
	ts = Z_StrDup(p->name);
	for (t = ts; *t; t++)
	{
		switch(*t)
		{
		case '/':
		case '?':
		case '<':
		case '>':
		case '\\':
		case ':':
		case '*':
		case '|':
		case '\"':
		case '.':
			*t = '_';
			break;
		default:
			break;
		}
	}
	if (*ts)
	{
		if (*p->gamedir)
			destname = va("%s/%s.tmp", p->gamedir, ts);
		else
			destname = va("%s.tmp", ts);
	}
	else
		destname = va("%x.tmp", (unsigned int)(quintptr_t)p);
	Z_Free(ts);
	return Z_StrDup(destname);
}

static void Menu_Download_Got(struct dl_download *dl);
static void MD_StartADownload(void)
{
	vfsfile_t *tmpfile;
	char *temp;
//	char native[MAX_OSPATH];
	package_t *p;
	int simultaneous = 1;
	int i;

	for (p = availablepackages; p ; p=p->next)
	{
		if (p->download)
			simultaneous--;
	}

	for (p = availablepackages; p && simultaneous > 0; p=p->next)
	{
		if (p->trymirrors)
		{	//flagged for a (re?)download
			char *mirror = NULL;
			for (i = 0; i < countof(p->mirror); i++)
			{
				if (p->mirror[i] && (p->trymirrors & (1u<<i)))
				{
					mirror = p->mirror[i];
					p->trymirrors &= ~(1u<<i);
					break;
				}
			}
			if (!mirror)
			{	//erk...
				p->trymirrors = 0;

				for (i = 0; i < countof(p->mirror); i++)
					if (p->mirror[i])
						break;
				if (i == countof(p->mirror))
				{	//this appears to be a meta package with no download
					//just directly install it.
					p->flags &= ~(DPF_NATIVE|DPF_CACHED|DPF_CORRUPT);
					p->flags |= DPF_INSTALLED;
					PM_WriteInstalledPackages();
				}
				continue;
			}

			if (p->qhash && (p->flags & DPF_CACHED))
			{	//its in our cache directory, so lets just use that
				p->trymirrors = 0;
				p->flags |= DPF_INSTALLED;
				PM_WriteInstalledPackages();
				FS_ReloadPackFiles();
				continue;
			}


			temp = MD_GetTempName(p);

			//FIXME: we should lock in the temp path, in case the user foolishly tries to change gamedirs.

			FS_CreatePath(temp, p->fsroot);
			switch (p->extract)
			{
			case EXTRACT_ZIP:
			case EXTRACT_COPY:
				tmpfile = FS_OpenVFS(temp, "wb", p->fsroot);
				break;
#ifdef AVAIL_XZDEC
			case EXTRACT_XZ:
				{
					vfsfile_t *raw;
					raw = FS_OpenVFS(temp, "wb", p->fsroot);
					tmpfile = FS_XZ_DecompressWriteFilter(raw);
					if (!tmpfile)
						VFS_CLOSE(raw);
				}
				break;
#endif
#ifdef AVAIL_GZDEC
			case EXTRACT_GZ:
				{
					vfsfile_t *raw;
					raw = FS_OpenVFS(temp, "wb", p->fsroot);
					tmpfile = FS_GZ_DecompressWriteFilter(raw, true);
					if (!tmpfile)
						VFS_CLOSE(raw);
				}
				break;
#endif
			default:
				Con_Printf("decompression method not supported\n");
				continue;
			}

			if (tmpfile)
				p->download = HTTP_CL_Get(mirror, NULL, Menu_Download_Got);
			if (p->download)
			{
				Con_Printf("Downloading %s\n", p->fullname);
				p->download->file = tmpfile;
				p->download->user_ctx = temp;

				DL_CreateThread(p->download, NULL, NULL);
			}
			else
			{
				Con_Printf("Unable to download %s\n", p->fullname);
				p->flags &= ~DPF_MARKED;	//can't do it.
				if (tmpfile)
					VFS_CLOSE(tmpfile);
				FS_Remove(temp, p->fsroot);
			}

			simultaneous--;
		}
	}
}

static qboolean MD_ApplyDownloads (union menuoption_s *mo,struct menu_s *m,int key)
{
	if (key == K_ENTER || key == K_KP_ENTER || key == K_MOUSE1)
	{
		package_t *p, **link;

#ifdef HAVEAUTOUPDATE
		if (autoupdatesetting != UPD_UNSUPPORTED)
		{
			Sys_SetAutoUpdateSetting(autoupdatesetting);
			autoupdatesetting = UPD_UNSUPPORTED;
		}
#endif

		//delete any that don't exist
		for (link = &availablepackages; *link ; )
		{
			p = *link;
			if ((p->flags & DPF_PURGE) || (!(p->flags&DPF_MARKED) && (p->flags&DPF_INSTALLED)))
			{	//if we don't want it but we have it anyway:
				qboolean reloadpacks = false;
				struct packagedep_s *dep;
				for (dep = p->deps; dep; dep = dep->next)
				{
					if (dep->dtype == DEP_FILE)
					{
						if (!reloadpacks)
						{
							char ext[8];
							COM_FileExtension(dep->name, ext, sizeof(ext));
							if (!stricmp(ext, "pak") || !stricmp(ext, "pk3"))
							{
								reloadpacks = true;
								FS_UnloadPackFiles();
							}
						}
						if (*p->gamedir)
						{
							char *f = va("%s/%s", p->gamedir, dep->name);
							char temp[MAX_OSPATH];
							if (p->qhash && FS_GenCachedPakName(f, p->qhash, temp, sizeof(temp)) && PM_CheckFile(temp, p->fsroot))
							{
								if (p->flags & DPF_PURGE)
									FS_Remove(temp, p->fsroot);
							}
							else
								FS_Remove(va("%s/%s", p->gamedir, dep->name), p->fsroot);
						}
						else
							FS_Remove(dep->name, p->fsroot);
					}
				}

				p->flags &= ~(DPF_NATIVE|DPF_CACHED|DPF_CORRUPT|DPF_PURGE|DPF_INSTALLED);
				PM_ValidatePackage(p);
				PM_WriteInstalledPackages();

				if (reloadpacks)
					FS_ReloadPackFiles();

				if (p->flags & DPF_FORGETONUNINSTALL)
				{
					if (p->alternative)
					{	//replace it with its alternative package
						*p->link = p->alternative;
						p->alternative->alternative = p->alternative->next;
						if (p->alternative->alternative)
							p->alternative->alternative->link = &p->alternative->alternative;
						p->alternative->next = p->next;
					}
					else
					{	//just remove it from the list.
						*p->link = p->next;
						if (p->next)
							p->next->link = p->link;
					}

//FIXME: the menu(s) hold references to packages, so its not safe to purge them
					p->flags |= DPF_HIDDEN;
//					BZ_Free(p);

					continue;
				}
			}

			link = &(*link)->next;
		}

		//and flag any new/updated ones for a download
		for (p = availablepackages; p ; p=p->next)
		{
			if ((p->flags&DPF_MARKED) && !(p->flags&DPF_INSTALLED) && !p->download)
				p->trymirrors = ~0u;
		}
		MD_StartADownload();	//and try to do those downloads.
		return true;
	}
	return false;
}

static qboolean MD_MarkUpdatesButton (union menuoption_s *mo,struct menu_s *m,int key)
{
	if (key == K_ENTER || key == K_KP_ENTER || key == K_MOUSE1)
	{
		PM_MarkUpdates();
		return true;
	}
	return false;
}
static qboolean MD_RevertUpdates (union menuoption_s *mo,struct menu_s *m,int key)
{
	if (key == K_ENTER || key == K_KP_ENTER || key == K_MOUSE1)
	{
		package_t *p;
		for (p = availablepackages; p; p = p->next)
		{
			if (p->flags & DPF_INSTALLED)
				p->flags |= DPF_MARKED;
			else
				p->flags &= ~DPF_MARKED;
		}
		return true;
	}
	return false;
}

void M_AddItemsToDownloadMenu(menu_t *m)
{
	char path[MAX_QPATH];
	int y;
	package_t *p;
	menucustom_t *c;
	char *slash;
	menuoption_t *mo;
	dlmenu_t *info = m->data;
	int prefixlen;
	p = availablepackages;

	prefixlen = strlen(info->pathprefix);
	y = 48;
	
	MC_AddCommand(m, 0, 170, y, "Apply", MD_ApplyDownloads);
	y+=8;
	MC_AddCommand(m, 0, 170, y, "Back", MD_PopMenu);
	y+=8;
	if (!prefixlen)
	{
		MC_AddCommand(m, 0, 170, y, "Mark Updates", MD_MarkUpdatesButton);
		y+=8;

		MC_AddCommand(m, 0, 170, y, "Revert Updates", MD_RevertUpdates);
		y+=8;
	}
	if (!prefixlen)
	{
		c = MC_AddCustom(m, 0, y, p, 0);
		c->draw = MD_AutoUpdate_Draw;
		c->key = MD_AutoUpdate_Key;
		c->common.width = 320;
		c->common.height = 8;
		y += 8;
	}

	y+=4;	//small gap
	for (p = availablepackages; p; p = p->next)
	{
		if (strncmp(p->fullname, info->pathprefix, prefixlen))
			continue;
		if ((p->flags & DPF_HIDDEN) && (p->arch || !(p->flags & DPF_INSTALLED)))
			continue;

		slash = strchr(p->fullname+prefixlen, '/');
		if (slash)
		{
			Q_strncpyz(path, p->fullname, MAX_QPATH);
			slash = strchr(path+prefixlen, '/');
			if (slash)
				*slash = '\0';

			for (mo = m->options; mo; mo = mo->common.next)
				if (mo->common.type == mt_button)
					if (!strcmp(mo->button.text, path + prefixlen))
						break;
			if (!mo)
			{
				menubutton_t *b = MC_AddConsoleCommand(m, 6*8, 170, y, path+prefixlen, va("menu_download \"%s/\"", path));
				y += 8;

				if (!m->selecteditem)
					m->selecteditem = (menuoption_t*)b;
			}
		}
		else
		{
			c = MC_AddCustom(m, 0, y, p, downloadablessequence);
			c->draw = MD_Draw;
			c->key = MD_Key;
			c->common.width = 320;
			c->common.height = 8;
			c->common.tooltip = p->description;
			y += 8;

			if (!m->selecteditem)
				m->selecteditem = (menuoption_t*)c;
		}
	}
}

#include "shader.h"
void M_Download_UpdateStatus(struct menu_s *m)
{
	dlmenu_t *info = m->data;
	int i;

	if (info->downloadablessequence != downloadablessequence)
	{
		while(m->options)
		{
			menuoption_t *op = m->options;
			m->options = op->common.next;
			if (op->common.iszone)
				Z_Free(op);
		}
		m->cursoritem = m->selecteditem = NULL;

		info->populated = false;
		MC_AddWhiteText(m, 24, 170, 8, "Downloads", false);
		MC_AddWhiteText(m, 16, 170, 24, "^Ue01d^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01f", false);

		//FIXME: should probably reselect the previous selected item. lets just assume everyone uses a mouse...
	}

	if (!info->populated)
	{
		for (i = 0; i < numdownloadablelists; i++)
		{
			if (!downloadablelist[i].received)
			{
				Draw_FunStringWidth(0, vid.height - 8, "Querying for package list", vid.width, 2, false);
				return;
			}
		}

		info->populated = true;
		M_AddItemsToDownloadMenu(m);
	}

	if (m->selecteditem && m->selecteditem->common.type == mt_custom && m->selecteditem->custom.dptr)
	{
		package_t *p = m->selecteditem->custom.dptr;
		if (p->previewimage)
		{
			shader_t *sh = R_RegisterPic(p->previewimage);
			if (R_GetShaderSizes(sh, NULL, NULL, false) > 0)
				R2D_Image(0, 0, vid.width, vid.height, 0, 0, 1, 1, sh);
		}
	}
}

static int QDECL MD_ExtractFiles(const char *fname, qofs_t fsize, time_t mtime, void *parm, searchpathfuncs_t *spath)
{	//this is gonna suck. threading would help, but gah.
	package_t *p = parm;
	flocation_t loc;
	if (fname[strlen(fname)-1] == '/')
	{	//directory.

	}
	else if (spath->FindFile(spath, &loc, fname, NULL) && loc.len < 0x80000000u)
	{
		char *f = malloc(loc.len);
		const char *n;
		if (f)
		{
			spath->ReadFile(spath, &loc, f);
			if (*p->gamedir)
				n = va("%s/%s", p->gamedir, fname);
			else
				n = fname;
			if (FS_WriteFile(n, f, loc.len, p->fsroot))
				p->flags |= DPF_NATIVE|DPF_INSTALLED;
			free(f);

			//keep track of the installed files, so we can delete them properly after.
			PM_AddDep(p, DEP_FILE, fname);
		}
	}
	return 1;
}

static void Menu_Download_Got(struct dl_download *dl)
{
	qboolean successful = dl->status == DL_FINISHED;
	package_t *p;
	char *tempname = dl->user_ctx;

	for (p = availablepackages; p ; p=p->next)
	{
		if (p->download == dl)
			break;
	}

	if (dl->file)
	{
		VFS_CLOSE(dl->file);
		dl->file = NULL;
	}

	if (p)
	{
		char ext[8];
		char *destname;
		struct packagedep_s *dep;
		p->download = NULL;

		if (!successful)
		{
			Con_Printf("Couldn't download %s (from %s)\n", p->name, dl->url);
			FS_Remove (tempname, p->fsroot);
			Z_Free(tempname);
			MD_StartADownload();
			return;
		}

		if (p->extract == EXTRACT_ZIP)
		{
			vfsfile_t *f = FS_OpenVFS(tempname, "rb", p->fsroot);
			if (f)
			{
				searchpathfuncs_t *archive = FSZIP_LoadArchive(f, tempname, NULL);
				if (archive)
				{
					p->flags &= ~(DPF_NATIVE|DPF_CACHED|DPF_CORRUPT|DPF_INSTALLED);
					archive->EnumerateFiles(archive, "*", MD_ExtractFiles, p);
					archive->EnumerateFiles(archive, "*/*", MD_ExtractFiles, p);
					archive->EnumerateFiles(archive, "*/*/*", MD_ExtractFiles, p);
					archive->EnumerateFiles(archive, "*/*/*/*", MD_ExtractFiles, p);
					archive->EnumerateFiles(archive, "*/*/*/*/*", MD_ExtractFiles, p);
					archive->EnumerateFiles(archive, "*/*/*/*/*/*", MD_ExtractFiles, p);
					archive->EnumerateFiles(archive, "*/*/*/*/*/*/*", MD_ExtractFiles, p);
					archive->EnumerateFiles(archive, "*/*/*/*/*/*/*/*", MD_ExtractFiles, p);
					archive->EnumerateFiles(archive, "*/*/*/*/*/*/*/*/*", MD_ExtractFiles, p);
					archive->ClosePath(archive);

					PM_WriteInstalledPackages();

//					if (!stricmp(ext, "pak") || !stricmp(ext, "pk3"))
//						FS_ReloadPackFiles();
				}
				else
					VFS_CLOSE(f);
			}
			PM_ValidatePackage(p);

			FS_Remove (tempname, p->fsroot);
			Z_Free(tempname);
			MD_StartADownload();
			return;
		}
		else
		{
			for (dep = p->deps; dep; dep = dep->next)
			{
				unsigned int nfl;
				if (dep->dtype != DEP_FILE)
					continue;

				COM_FileExtension(dep->name, ext, sizeof(ext));
				if (!stricmp(ext, "pak") || !stricmp(ext, "pk3"))
					FS_UnloadPackFiles();	//we reload them after
				if ((!stricmp(ext, "dll") || !stricmp(ext, "so")) && !Q_strncmp(dep->name, "fteplug_", 8))
					Cmd_ExecuteString(va("plug_close %s\n", dep->name), RESTRICT_LOCAL);	//try to purge plugins so there's no files left open

				nfl = DPF_NATIVE;
				if (*p->gamedir)
				{
					char temp[MAX_OSPATH];
					destname = va("%s/%s", p->gamedir, dep->name);
					if (p->qhash && FS_GenCachedPakName(destname, p->qhash, temp, sizeof(temp)))
					{
						nfl = DPF_CACHED;
						destname = va("%s", temp);
					}
				}
				else
					destname = dep->name;
				nfl |= DPF_INSTALLED | (p->flags & ~(DPF_CACHED|DPF_NATIVE|DPF_CORRUPT));
				FS_CreatePath(destname, p->fsroot);
				if (FS_Remove(destname, p->fsroot))
					;
				if (!FS_Rename2(tempname, destname, p->fsroot, p->fsroot))
				{
					//error!
					Con_Printf("Couldn't rename %s to %s. Removed instead.\n", tempname, destname);
					FS_Remove (tempname, p->fsroot);
				}
				else
				{	//success!
					Con_Printf("Downloaded %s (to %s)\n", p->name, destname);
					p->flags = nfl;
					PM_WriteInstalledPackages();
				}

				PM_ValidatePackage(p);

				if (!stricmp(ext, "pak") || !stricmp(ext, "pk3"))
					FS_ReloadPackFiles();
				if ((!stricmp(ext, "dll") || !stricmp(ext, "so")) && !Q_strncmp(dep->name, "fteplug_", 8))
					Cmd_ExecuteString(va("plug_load %s\n", dep->name), RESTRICT_LOCAL);

				Z_Free(tempname);
				MD_StartADownload();
				return;
			}
		}
		Con_Printf("menu_download: %s has no filename info\n", p->name);
	}
	else
		Con_Printf("menu_download: Can't figure out where %s came from (url: %s)\n", dl->localname, dl->url);

	FS_Remove (tempname, FS_GAMEONLY);
	Z_Free(tempname);
	MD_StartADownload();
}

void Menu_DownloadStuff_f (void)
{
	menu_t *menu;
	dlmenu_t *info;

	Key_Dest_Add(kdm_emenu);
	m_state = m_complex;

	menu = M_CreateMenu(sizeof(dlmenu_t));
	info = menu->data;

	menu->predraw = M_Download_UpdateStatus;
	info->downloadablessequence = downloadablessequence;


	Q_strncpyz(info->pathprefix, Cmd_Argv(1), sizeof(info->pathprefix));
	if (!*info->pathprefix || !loadedinstalled)
		PM_UpdatePackageList(false, true);

	MC_AddWhiteText(menu, 24, 170, 8, "Downloads", false);
	MC_AddWhiteText(menu, 16, 170, 24, "^Ue01d^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01e^Ue01f", false);
}

//should only be called AFTER the filesystem etc is inited.
void Menu_Download_Update(void)
{
	if (Sys_GetAutoUpdateSetting() == UPD_OFF || Sys_GetAutoUpdateSetting() == UPD_REVERT)
		return;

	PM_UpdatePackageList(true, 2);
}

#endif

#else
void Menu_DownloadStuff_f (void)
{
	Con_Printf("Download menu not implemented in this build\n");
}
void Menu_Download_Update(void)
{
}
void PM_LoadPackages(searchpath_t **oldpaths, const char *parent_pure, const char *parent_logical, searchpath_t *search, unsigned int loadstuff, int minpri, int maxpri)
{
}
void PM_Shutdown(void)
{
}
#endif
