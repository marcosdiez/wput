/* Parsing FTP `ls' output.
   Copyright (C) 1995, 1996, 1997, 2000, 2001
   Free Software Foundation, Inc. 

This file is part of GNU Wget.

GNU Wget is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

GNU Wget is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Wget; if not, write to the Free Software
Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

In addition, as a special exception, the Free Software Foundation
gives permission to link the code of its release of Wget with the
OpenSSL project's "OpenSSL" library (or with modified versions of it
that use the same license as the "OpenSSL" library), and distribute
the linked executables.  You must obey the GNU General Public License
in all respects for all of the code used other than "OpenSSL".  If you
modify this file, you may extend this exception to your version of the
file, but you are not obligated to do so.  If you do not wish to do
so, delete this exception statement from your version.  */

#include "config.h"
#define _XOPEN_SOURCE
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef HAVE_STRING_H
# include <string.h>
#else
# ifdef HAVE_STRINGS_H
#  include <strings.h>
# endif
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#include <sys/types.h>
#include <errno.h>

#include "wget.h"
#include "utils.h"
#include "ftp.h"
#include "url.h"

/* 2004-12-09 SMS.
 * Added some conditionality to allow easier use within Wput.
 */

#ifndef WPUT

/* Converts symbolic permissions to number-style ones, e.g. string
   rwxr-xr-x to 755.  For now, it knows nothing of
   setuid/setgid/sticky.  ACLs are ignored.  */
static int
symperms (const char *s)
{
  int perms = 0, i;

  if (strlen (s) < 9)
    return 0;
  for (i = 0; i < 3; i++, s += 3)
    {
      perms <<= 3;
      perms += (((s[0] == 'r') << 2) + ((s[1] == 'w') << 1) +
		(s[2] == 'x' || s[2] == 's'));
    }
  return perms;
}

#endif /* ndef WPUT */

/* Cleans a line of text so that it can be consistently parsed. Destroys
   <CR> and <LF> in case that thay occur at the end of the line and
   replaces all <TAB> character with <SPACE>. Returns the length of the
   modified line. */
static int
clean_line(char *line)
{
  int len = strlen (line);
  if (!len) return 0; 
  if (line[len - 1] == '\n')
    line[--len] = '\0';
  if (line[len - 1] == '\r')
    line[--len] = '\0';
  for ( ; *line ; line++ ) if (*line == '\t') *line = ' '; 
  return len;
}

/* Convert the Un*x-ish style directory listing stored in FILE to a
   linked list of fileinfo (system-independent) entries.  The contents
   of FILE are considered to be produced by the standard Unix `ls -la'
   output (whatever that might be).  BSD (no group) and SYSV (with
   group) listings are handled.

   The time stamps are stored in a separate variable, time_t
   compatible (I hope).  The timezones are ignored.  */
static struct fileinfo *
ftp_parse_unix_ls (const char *file, int ignore_perms)
{
  FILE *fp;
  static const char *months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
  };
  int next, len, i, error, ignore;
  int year, month, day;		/* for time analysis */
  int hour, min, sec;
  struct tm timestruct, *tnow;
  time_t timenow;

  char *line, *tok;		/* tokenizer */
  struct fileinfo *dir, *l, cur; /* list creation */

  memset(&timenow, '\0', sizeof(timenow));
  memset(&timestruct, '\0', sizeof(timestruct));
  memset(&cur, '\0', sizeof(cur));

  fp = fopen (file, "r");
  if (!fp)
    {
      logprintf (LOG_NOTQUIET, "%s: %s\n", file, strerror (errno));
      return NULL;
    }
  dir = l = NULL;

  /* Line loop to end of file: */
  while ((line = read_whole_line (fp)))
    {
      len = clean_line (line);
      /* Skip if total...  */
      if (!strncasecmp (line, "total", 5))
	{
	  xfree (line);
	  continue;
	}
      /* Get the first token (permissions).  */
      tok = strtok (line, " ");
      if (!tok)
	{
	  xfree (line);
	  continue;
	}

      cur.name = NULL;
      cur.linkto = NULL;

      /* Decide whether we deal with a file or a directory.  */
      switch (*tok)
	{
	case '-':
	  cur.type = FT_PLAINFILE;
	  DEBUGP (("PLAINFILE; "));
	  break;
	case 'd':
	  cur.type = FT_DIRECTORY;
	  DEBUGP (("DIRECTORY; "));
	  break;
	case 'l':
	  cur.type = FT_SYMLINK;
	  DEBUGP (("SYMLINK; "));
	  break;
	default:
	  cur.type = FT_UNKNOWN;
	  DEBUGP (("UNKNOWN; "));
	  break;
	}

#ifndef WPUT
      if (ignore_perms)
	{
 	  switch (cur.type)
 	    {
 	    case FT_PLAINFILE:
 	      cur.perms = 0644;
 	      break;
 	    case FT_DIRECTORY:
 	      cur.perms = 0755;
 	      break;
 	    default:
 	      /*cur.perms = 1023;*/	/* #### What is this?  --hniksic */
	      cur.perms = 0644;
 	    }
 	  DEBUGP (("implicit perms %0o; ", cur.perms));
	}
       else
         {
	   cur.perms = symperms (tok + 1);
	   DEBUGP (("perms %0o; ", cur.perms));
	 }
#endif /* ndef WPUT */

      error = ignore = 0;       /* Erroneous and ignoring entries are
				   treated equally for now.  */
      year = hour = min = sec = 0; /* Silence the compiler.  */
      month = day = 0;
      next = -1;
      /* While there are tokens on the line, parse them.  Next is the
	 number of tokens left until the filename.

	 Use the month-name token as the "anchor" (the place where the
	 position wrt the file name is "known").  When a month name is
	 encountered, `next' is set to 5.  Also, the preceding
	 characters are parsed to get the file size.

	 This tactic is quite dubious when it comes to
	 internationalization issues (non-English month names), but it
	 works for now.  */
      while ((tok = strtok (NULL, " ")))
	{
	  --next;
	  if (next < 0)		/* a month name was not encountered */
	    {
	      for (i = 0; i < 12; i++)
		if (!strcmp (tok, months[i]))
		  break;
	      /* If we got a month, it means the token before it is the
		 size, and the filename is three tokens away.  */
	      if (i != 12)
		{
		  char *t = tok - 2;
		  long mul = 1;

		  for (cur.size = 0; t > line && ISDIGIT (*t); mul *= 10, t--)
		    cur.size += mul * (*t - '0');
		  if (t == line)
		    {
		      /* Something is seriously wrong.  */
		      error = 1;
		      break;
		    }
		  month = i;
		  next = 5;
		  DEBUGP (("month: %s; ", months[month]));
		}
	    }
	  else if (next == 4)	/* days */
	    {
	      if (tok[1])	/* two-digit... */
		day = 10 * (*tok - '0') + tok[1] - '0';
	      else		/* ...or one-digit */
		day = *tok - '0';
	      DEBUGP (("day: %d; ", day));
	    }
	  else if (next == 3)
	    {
	      /* This ought to be either the time, or the year.  Let's
		 be flexible!

		 If we have a number x, it's a year.  If we have x:y,
		 it's hours and minutes.  If we have x:y:z, z are
		 seconds.  */
	      year = 0;
	      min = hour = sec = 0;
	      /* We must deal with digits.  */
	      if (ISDIGIT (*tok))
		{
		  /* Suppose it's year.  */
		  for (; ISDIGIT (*tok); tok++)
		    year = (*tok - '0') + 10 * year;
		  if (*tok == ':')
		    {
		      /* This means these were hours!  */
		      hour = year;
		      year = 0;
		      ++tok;
		      /* Get the minutes...  */
		      for (; ISDIGIT (*tok); tok++)
			min = (*tok - '0') + 10 * min;
		      if (*tok == ':')
			{
			  /* ...and the seconds.  */
			  ++tok;
			  for (; ISDIGIT (*tok); tok++)
			    sec = (*tok - '0') + 10 * sec;
			}
		    }
		}
	      if (year)
		{
		DEBUGP (("year: %d (no tm); ", year));
		}
	      else
		{
		DEBUGP (("time: %02d:%02d:%02d (no yr); ", hour, min, sec));
		}
	    }
	  else if (next == 2)    /* The file name */
	    {
	      int fnlen;
	      char *p;

	      /* Since the file name may contain a SPC, it is possible
		 for strtok to handle it wrong.  */
	      fnlen = strlen (tok);
	      if (fnlen < len - (tok - line))
		{
		  /* So we have a SPC in the file name.  Restore the
		     original.  */
		  tok[fnlen] = ' ';
		  /* If the file is a symbolic link, it should have a
		     ` -> ' somewhere.  */
		  if (cur.type == FT_SYMLINK)
		    {
		      p = strstr (tok, " -> ");
		      if (!p)
			{
			  error = 1;
			  break;
			}
		      cur.linkto = xstrdup (p + 4);
		      DEBUGP (("link to: %s\n", cur.linkto));
		      /* And separate it from the file name.  */
		      *p = '\0';
		    }
		}
	      /* If we have the filename, add it to the list of files or
		 directories.  */
	      /* "." and ".." are an exception!  */
	      if (!strcmp (tok, ".") || !strcmp (tok, ".."))
		{
		  DEBUGP (("\nIgnoring `.' and `..'; "));
		  ignore = 1;
		  break;
		}
	      /* Some FTP sites choose to have ls -F as their default
		 LIST output, which marks the symlinks with a trailing
		 `@', directory names with a trailing `/' and
		 executables with a trailing `*'.  This is no problem
		 unless encountering a symbolic link ending with `@',
		 or an executable ending with `*' on a server without
		 default -F output.  I believe these cases are very
		 rare.  */
	      fnlen = strlen (tok); /* re-calculate `fnlen' */
	      cur.name = (char *)xmalloc (fnlen + 1);
	      memcpy (cur.name, tok, fnlen + 1);
	      if (fnlen)
		{
		  if (cur.type == FT_DIRECTORY && cur.name[fnlen - 1] == '/')
		    {
		      cur.name[fnlen - 1] = '\0';
		      DEBUGP (("trailing `/' on dir.\n"));
		    }
		  else if (cur.type == FT_SYMLINK && cur.name[fnlen - 1] == '@')
		    {
		      cur.name[fnlen - 1] = '\0';
		      DEBUGP (("trailing `@' on link.\n"));
		    }
		  else if (cur.type == FT_PLAINFILE
			   && (cur.perms & 0111)
			   && cur.name[fnlen - 1] == '*')
		    {
		      cur.name[fnlen - 1] = '\0';
		      DEBUGP (("trailing `*' on exec.\n"));
		    }
		} /* if (fnlen) */
	      else
		error = 1;
	      break;
	    }
	  else
	    abort ();
	} /* while */

      if (!cur.name || (cur.type == FT_SYMLINK && !cur.linkto))
	error = 1;

      DEBUGP (("\n"));

      if (error || ignore)
	{
	  DEBUGP (("Skipping.\n"));
	  FREE_MAYBE (cur.name);
	  FREE_MAYBE (cur.linkto);
	  xfree (line);
	  continue;
	}

      if (!dir)
	{
	  l = dir = (struct fileinfo *)xmalloc (sizeof (struct fileinfo));
	  memcpy (l, &cur, sizeof (cur));
	  l->prev = l->next = NULL;
	}
      else
	{
	  cur.prev = l;
	  l->next = (struct fileinfo *)xmalloc (sizeof (struct fileinfo));
	  l = l->next;
	  memcpy (l, &cur, sizeof (cur));
	  l->next = NULL;
	}
      /* Get the current time.  */
      timenow = time (NULL);
      tnow = localtime (&timenow);
      /* Build the time-stamp (the idea by zaga@fly.cc.fer.hr).  */
      timestruct.tm_sec   = sec;
      timestruct.tm_min   = min;
      timestruct.tm_hour  = hour;
      timestruct.tm_mday  = day;
      timestruct.tm_mon   = month;
      if (year == 0)
	{
	  /* Some listings will not specify the year if it is "obvious"
	     that the file was from the previous year.  E.g. if today
	     is 97-01-12, and you see a file of Dec 15th, its year is
	     1996, not 1997.  Thanks to Vladimir Volovich for
	     mentioning this!  */
	  if (month > tnow->tm_mon)
	    timestruct.tm_year = tnow->tm_year - 1;
	  else
	    timestruct.tm_year = tnow->tm_year;
	}
      else
	timestruct.tm_year = year;
      if (timestruct.tm_year >= 1900)
	timestruct.tm_year -= 1900;
      timestruct.tm_wday  = 0;
      timestruct.tm_yday  = 0;
      timestruct.tm_isdst = -1;
      l->tstamp = mktime (&timestruct); /* store the time-stamp */

      xfree (line);
    }

  fclose (fp);
  return dir;
}

static struct fileinfo *
ftp_parse_winnt_ls (const char *file)
{
  FILE *fp;
  int len;
  int year, month, day;         /* for time analysis */
  int hour, min;
  struct tm timestruct;

  char *line, *tok;             /* tokenizer */
  struct fileinfo *dir, *l, cur; /* list creation */

  fp = fopen (file, "r");
  if (!fp)
    {
      logprintf (LOG_NOTQUIET, "%s: %s\n", file, strerror (errno));
      return NULL;
    }
  dir = l = NULL;

  /* Line loop to end of file: */
  while ((line = read_whole_line (fp)))
    {
      len = clean_line (line);

      /* Extracting name is a bit of black magic and we have to do it
         before `strtok' inserted extra \0 characters in the line
         string. For the moment let us just suppose that the name starts at
         column 39 of the listing. This way we could also recognize
         filenames that begin with a series of space characters (but who
         really wants to use such filenames anyway?). */
      if (len < 40) continue;
      tok = line + 39;
      cur.name = xstrdup(tok);
      DEBUGP(("Name: '%s'\n", cur.name));

      /* First column: mm-dd-yy. Should atoi() on the month fail, january
	 will be assumed.  */
      tok = strtok(line, "-");
      month = atoi(tok) - 1;
      if (month < 0) month = 0;
      tok = strtok(NULL, "-");
      day = atoi(tok);
      tok = strtok(NULL, " ");
      year = atoi(tok);
      /* Assuming the epoch starting at 1.1.1970 */
      if (year <= 70) year += 100;

      /* Second column: hh:mm[AP]M, listing does not contain value for
         seconds */
      tok = strtok(NULL,  ":");
      hour = atoi(tok);
      tok = strtok(NULL,  "M");
      min = atoi(tok);
      /* Adjust hour from AM/PM. Just for the record, the sequence goes
         11:00AM, 12:00PM, 01:00PM ... 11:00PM, 12:00AM, 01:00AM . */
      tok+=2;
      if (hour == 12)  hour  = 0;
      if (*tok == 'P') hour += 12;

      DEBUGP(("YYYY/MM/DD HH:MM - %d/%02d/%02d %02d:%02d\n", 
              year+1900, month, day, hour, min));
      
      /* Build the time-stamp (copy & paste from above) */
      timestruct.tm_sec   = 0;
      timestruct.tm_min   = min;
      timestruct.tm_hour  = hour;
      timestruct.tm_mday  = day;
      timestruct.tm_mon   = month;
      timestruct.tm_year  = year;
      timestruct.tm_wday  = 0;
      timestruct.tm_yday  = 0;
      timestruct.tm_isdst = -1;
      cur.tstamp = mktime (&timestruct); /* store the time-stamp */

      DEBUGP(("Timestamp: %ld\n", cur.tstamp));

      /* Third column: Either file length, or <DIR>. We also set the
         permissions (guessed as 0644 for plain files and 0755 for
         directories as the listing does not give us a clue) and filetype
         here. */
      tok = strtok(NULL, " ");
      while (*tok == '\0')  tok = strtok(NULL, " ");
      if (*tok == '<')
	{
	  cur.type  = FT_DIRECTORY;
	  cur.size  = 0;
	  cur.perms = 0755;
	  DEBUGP(("Directory\n"));
	}
      else
	{
	  cur.type  = FT_PLAINFILE;
	  cur.size  = atoi(tok);
	  cur.perms = 0644;
	  DEBUGP(("File, size %ld bytes\n", cur.size));
	}

      cur.linkto = NULL;

      /* And put everything into the linked list */
      if (!dir)
	{
	  l = dir = (struct fileinfo *)xmalloc (sizeof (struct fileinfo));
	  memcpy (l, &cur, sizeof (cur));
	  l->prev = l->next = NULL;
	}
      else
	{
	  cur.prev = l;
	  l->next = (struct fileinfo *)xmalloc (sizeof (struct fileinfo));
	  l = l->next;
	  memcpy (l, &cur, sizeof (cur));
	  l->next = NULL;
	}

      xfree(line);
    }

  fclose(fp);
  return dir;
}

/* Convert the VMS-style directory listing stored in "file" to a
   linked list of fileinfo (system-independent) entries.  The contents
   of FILE are considered to be produced by the standard VMS
   "DIRECTORY [/SIZE [= ALL]] /DATE [/OWNER] [/PROTECTION]" command,
   more or less.  (Different VMS FTP servers may have different headers,
   and may not supply the same data, but all should be subsets of this.)

   VMS normally provides local (server) time and date information. 
   Define the logical name or environment variable
   "WGET_TIMEZONE_DIFFERENTIAL" (seconds) to adjust the receiving local
   times if different from the remote local times.
*/

#define VMS_DEFAULT_PROT_FILE 0644
#define VMS_DEFAULT_PROT_DIR 0755

static struct fileinfo *
ftp_parse_vms_ls (const char *file)
{
  FILE *fp;
  int dt, i, j, len;
  int perms;
  time_t timenow;
  struct tm timestruct;
  char date_str[ 32];

  char *line, *tok;		 /* tokenizer */
  struct fileinfo *dir, *l, cur; /* list creation */

  fp = fopen (file, "r");
  if (!fp)
    {
      logprintf (LOG_NOTQUIET, "%s: %s\n", file, strerror (errno));
      return NULL;
    }
  dir = l = NULL;

  /* Skip blank lines, Directory heading, and more blank lines. */

  j = 0; /* Expecting initial blank line(s). */
  while (1)
    {
      line = read_whole_line (fp);
      if (line == NULL)
        {
        break;
        }
      else
        {
          i = clean_line (line);
          if (i <= 0)
            {
              xfree (line); /* Free useless line storage. */
              continue; /* Blank line.  Keep looking. */
            }
          else
            {
              if ((j == 0) && (line[ i- 1] == ']'))
                {
                  /* Found Directory heading line.  Next non-blank line
                  is significant.
                  */
                  j = 1;
                }
              else if (!strncmp (line, "Total of ", 9))
                {
                  /* Found "Total of ..." footing line.  No valid data
                     will follow (empty directory).
                  */
                  xfree (line); /* Free useless line storage. */
                  line = NULL; /* Arrange for early exit. */
                  break;
                }
              else
                {
                  break; /* Must be significant data. */
                }
            }
          xfree (line); /* Free useless line storage. */
        }
    }

  /* Read remainder of file until the next blank line or EOF. */

  while (line != NULL)
    {
      char *p;

      /* The first token is the file name.  After a long name, other
         data may be on the following line.  A valid directory name ends
         in ".DIR;1" (any case), although some VMS FTP servers may omit
         the version number (";1").
      */

      tok = strtok(line, " ");
      if (tok == NULL) tok = line;
      DEBUGP(("file name: '%s'\n", tok));

      /* Stripping the version number on a VMS system would be wrong.
         It may be foolish on a non-VMS system, too, but that's someone
         else's problem.  (Define PRESERVE_VMS_VERSIONS for proper
         operation on other operating systems.)
      */
#if (!defined( __VMS) && !defined( PRESERVE_VMS_VERSIONS))
      for (p = tok ; *p && *p != ';' ; p++);
      if (*p == ';') *p = '\0';
#endif /* (!defined( __VMS) && !defined( PRESERVE_VMS_VERSIONS)) */

      /* Differentiate between a directory and any other file.  A VMS
         listing may not include file protections (permissions).  Set a
         default permissions value (according to the file type), which
         may be overwritten later.  Store directory names without the
         ".DIR;1" file type and version number, as the plain name is
         what will work in a CWD command.
      */

      len = strlen( tok);
      if (!strncasecmp( (tok+ (len- 4)), ".DIR", 4))
        {
          *(tok+ (len -= 4)) = '\0'; /* Discard ".DIR". */
          cur.type  = FT_DIRECTORY;
          cur.perms = VMS_DEFAULT_PROT_DIR;
          DEBUGP(("Directory (nv)\n"));
        }
      else if (!strncasecmp( (tok+ (len- 6)), ".DIR;1", 6))
        {
          *(tok+ (len -= 6)) = '\0'; /* Discard ".DIR;1". */
          cur.type  = FT_DIRECTORY;
          cur.perms = VMS_DEFAULT_PROT_DIR;
          DEBUGP(("Directory (v)\n"));
        }
      else
        {
          cur.type  = FT_PLAINFILE;
          cur.perms = VMS_DEFAULT_PROT_FILE;
          DEBUGP(("File\n"));
        }
      cur.name = xstrdup(tok);
      DEBUGP(("Name: '%s'\n", cur.name));

      /* Null the date and time string. */
      *date_str = '\0';

      /* VMS lacks symbolic links. */
      cur.linkto = NULL;

      /* VMS reports file sizes in (512-byte) disk blocks, not bytes,
         hence useless for an integrity check based on byte-count.
         Set size to unknown.
      */
      cur.size  = 0;

      /* Get token 2, if any.  A long name may force all other data onto
         a second line.  If needed, read the second line.
      */

      tok = strtok(NULL, " ");
      if (tok == NULL) 
        {
          DEBUGP(("Getting additional line.\n"));
          xfree (line);
          line = read_whole_line (fp);
          if (!line)
            {
              DEBUGP(("EOF.  Leaving listing parser.\n"));
              break;
            }

          /* Second line must begin with " ".  Otherwise, it's a first
             line (and we may be confused).
          */
          if (i <= 0)
	    {
              /* Blank line.  End of significant file listing. */
              DEBUGP(("Blank line.  Leaving listing parser.\n"));
              xfree (line); /* Free useless line storage. */
	      break;
	    }
          else if (line[ 0] != ' ')
            {
              DEBUGP(("Non-blank in column 1.  Must be a new file name?\n"));
              continue;
            }
          else
            {
              tok = strtok (line, " ");
              if (tok == NULL)
                {
                  /* Unexpected non-empty but apparently blank line. */
                  DEBUGP(("Null token.  Leaving listing parser.\n"));
                  xfree (line); /* Free useless line storage. */
                  break;
                }
            }
        }

      /* Analyze tokens.  (Order is not significant, except date must
         precede time.)

         Size:       ddd or ddd/ddd (where "ddd" is a decimal number)
         Date:       DD-MMM-YYYY
         Time:       HH:MM or HH:MM:SS or HH:MM:SS.CC
         Owner:      [user] or [user,group]
         Protection: (ppp,ppp,ppp,ppp) (where "ppp" is "RWED" or some
                     subset thereof, for System, Owner, Group, World.

         If permission is lacking, info may be replaced by the string:
         "No privilege for attempted operation".
      */
      while (tok != NULL)
	{
	  DEBUGP (("Token: >%s<: ", tok));

	  if ((strlen( tok) < 12) && (strchr( tok, '-') != NULL))
	    {
	      /* Date. */
	      DEBUGP (("Date.\n"));
	      strcpy( date_str, tok);
	      strcat( date_str, " ");
	    }
	  else if ((strlen( tok) < 12) && (strchr( tok, ':') != NULL))
	    {
	      /* Time. */
	      DEBUGP (("Time. "));
	      strncat( date_str,
	       tok,
	       (sizeof( date_str)- strlen( date_str)- 1));
	      DEBUGP (("Date time: >%s<\n", date_str));
	    }
	  else if (strchr( tok, '[') != NULL)
	    {
	      /* Owner.  (Ignore.) */
	      DEBUGP (("Owner.\n"));
	    }
	  else if (strchr( tok, '(') != NULL)
	    {
	      /* Protections (permissions). */
	      perms = 0;
	      j = 0;
	      for (i = 0; i < strlen( tok); i++)
		{
		  switch (tok[ i])
		    {
		      case '(':
		        break;
		      case ')':
		        break;
		      case ',':
		        if (j == 0)
		          {
		            perms = 0;
		            j = 1;
		          }
		        else
		          {
		            perms <<= 3;
		          }
		        break;
		    case 'R':
		      perms |= 4;
		      break;
		    case 'W':
		      perms |= 2;
		      break;
		    case 'E':
		      perms |= 1;
		      break;
		    case 'D':
		      perms |= 2;
		      break;
		    }
		}
	      cur.perms = perms;
	      DEBUGP (("Prot.  perms = %0o.\n", cur.perms));
	    }
	  else
	    {
	      /* Nondescript.  Probably size(s), probably in blocks. 
                 Could be "No privilege ..." message.  (Ignore.)
              */
	      DEBUGP (("Ignored (size?).\n"));
	    }

	  tok = strtok (NULL, " ");
	}

      /* Tokens exhausted.  Interpret the data, and fill in the
         structure.
      */
      /* Fill tm timestruct according to date-time string.  Fractional
         seconds are ignored.  Default to current time, if conversion
         fails.
      */
      timenow = time( NULL);
      localtime_r( &timenow, &timestruct);
      strptime( date_str, "%d-%b-%Y %H:%M:%S", &timestruct);

      /* Convert struct tm local time to time_t local time. */
      timenow = mktime (&timestruct);
      /* Offset local time according to environment variable (seconds). */
      if ((tok = getenv( "WGET_TIMEZONE_DIFFERENTIAL")) != NULL)
        {
          dt = atoi( tok);
          DEBUGP (("Time differential = %d.\n", dt));
        }
      else
        {
          dt = 0;
        }

      if (dt >= 0)
        {
          timenow += dt;
        }
      else
        {
          timenow -= (-dt);
        }
      cur.tstamp = timenow; /* Store the time-stamp. */
      DEBUGP(("Timestamp: %ld\n", cur.tstamp));

      /* Add the data for this item to the linked list, */
      if (!dir)
        {
          l = dir = (struct fileinfo *)xmalloc (sizeof (struct fileinfo));
          memcpy (l, &cur, sizeof (cur));
          l->prev = l->next = NULL;
        }
      else
        {
          cur.prev = l;
          l->next = (struct fileinfo *)xmalloc (sizeof (struct fileinfo));
          l = l->next;
          memcpy (l, &cur, sizeof (cur));
          l->next = NULL;
        }

      /* Free old line storage.  Read a new line. */
      xfree (line);
      line = read_whole_line (fp);
      if (line != NULL)
        {
          i = clean_line (line);
          if (i <= 0)
	    {
              /* Blank line.  End of significant file listing. */
              xfree (line); /* Free useless line storage. */
	      break;
	    }
        }
    }

  fclose (fp);
  return dir;
}

/* This function switches between the correct parsing routine depending on
   the SYSTEM_TYPE. The system type should be based on the result of the
   "SYST" response of the FTP server. According to this repsonse we will
   use on of the three different listing parsers that cover the most of FTP
   servers used nowadays.

   Note that some VMS servers may respond with unexpected SYST strings. 
   It would be more reliable to determine the listing type by examining
   the listing itself, because it is, after all, the listing itself
   which will determine the success of the parsing attempted, not the
   SYST response.
*/

struct fileinfo *
ftp_parse_ls (const char *file, const enum stype system_type)
{
  switch (system_type)
    {
    case ST_UNIX:
      return ftp_parse_unix_ls (file, FALSE);
    case ST_WINNT:
      {
	/* Detect whether the listing is simulating the UNIX format */
	FILE *fp;
	int   c;

	fp = fopen (file, "r");
	if (!fp)
	{
	  logprintf (LOG_NOTQUIET, "%s: %s\n", file, strerror (errno));
	  return NULL;
        }
	c = fgetc(fp);
	fclose(fp);
	/* If the first character of the file is '0'-'9', it's WINNT
	   format. */
	if (c >= '0' && c <='9')
	  return ftp_parse_winnt_ls (file);
        else
          return ftp_parse_unix_ls (file, TRUE);
      }
    case ST_VMS:
      return ftp_parse_vms_ls (file);
    case ST_MACOS:
      return ftp_parse_unix_ls (file, TRUE);
    default:
      logprintf (LOG_NOTQUIET, _("\
Unsupported listing type, trying Unix listing parser.\n"));
      return ftp_parse_unix_ls (file, FALSE);
    }
}
#ifndef WPUT

/* Stuff for creating FTP index. */

/* The function creates an HTML index containing references to given
   directories and files on the appropriate host.  The references are
   FTP.  */
uerr_t
ftp_index (const char *file, struct url *u, struct fileinfo *f)
{
  FILE *fp;
  char *upwd;
  char *htclfile;		/* HTML-clean file name */

  if (!opt.dfp)
    {
      fp = fopen (file, "w");
      if (!fp)
	{
	  logprintf (LOG_NOTQUIET, "%s: %s\n", file, strerror (errno));
	  return FOPENERR;
	}
    }
  else
    fp = opt.dfp;
  if (u->user)
    {
      char *tmpu, *tmpp;        /* temporary, clean user and passwd */

      tmpu = url_escape (u->user);
      tmpp = u->passwd ? url_escape (u->passwd) : NULL;
      upwd = (char *)xmalloc (strlen (tmpu)
			     + (tmpp ? (1 + strlen (tmpp)) : 0) + 2);
      sprintf (upwd, "%s%s%s@", tmpu, tmpp ? ":" : "", tmpp ? tmpp : "");
      xfree (tmpu);
      FREE_MAYBE (tmpp);
    }
  else
    upwd = xstrdup ("");
  fprintf (fp, "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\n");
  fprintf (fp, "<html>\n<head>\n<title>");
  fprintf (fp, _("Index of /%s on %s:%d"), u->dir, u->host, u->port);
  fprintf (fp, "</title>\n</head>\n<body>\n<h1>");
  fprintf (fp, _("Index of /%s on %s:%d"), u->dir, u->host, u->port);
  fprintf (fp, "</h1>\n<hr>\n<pre>\n");
  while (f)
    {
      fprintf (fp, "  ");
      if (f->tstamp != -1)
	{
	  /* #### Should we translate the months?  Or, even better, use
	     ISO 8601 dates?  */
	  static char *months[] = {
	    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
	    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	  };
	  struct tm *ptm = localtime ((time_t *)&f->tstamp);

	  fprintf (fp, "%d %s %02d ", ptm->tm_year + 1900, months[ptm->tm_mon],
		  ptm->tm_mday);
	  if (ptm->tm_hour)
	    fprintf (fp, "%02d:%02d  ", ptm->tm_hour, ptm->tm_min);
	  else
	    fprintf (fp, "       ");
	}
      else
	fprintf (fp, _("time unknown       "));
      switch (f->type)
	{
	case FT_PLAINFILE:
	  fprintf (fp, _("File        "));
	  break;
	case FT_DIRECTORY:
	  fprintf (fp, _("Directory   "));
	  break;
	case FT_SYMLINK:
	  fprintf (fp, _("Link        "));
	  break;
	default:
	  fprintf (fp, _("Not sure    "));
	  break;
	}
      htclfile = html_quote_string (f->name);
      fprintf (fp, "<a href=\"ftp://%s%s:%d", upwd, u->host, u->port);
      if (*u->dir != '/')
	putc ('/', fp);
      fprintf (fp, "%s", u->dir);
      if (*u->dir)
	putc ('/', fp);
      fprintf (fp, "%s", htclfile);
      if (f->type == FT_DIRECTORY)
	putc ('/', fp);
      fprintf (fp, "\">%s", htclfile);
      if (f->type == FT_DIRECTORY)
	putc ('/', fp);
      fprintf (fp, "</a> ");
      if (f->type == FT_PLAINFILE)
	fprintf (fp, _(" (%s bytes)"), legible (f->size));
      else if (f->type == FT_SYMLINK)
	fprintf (fp, "-> %s", f->linkto ? f->linkto : "(nil)");
      putc ('\n', fp);
      xfree (htclfile);
      f = f->next;
    }
  fprintf (fp, "</pre>\n</body>\n</html>\n");
  xfree (upwd);
  if (!opt.dfp)
    fclose (fp);
  else
    fflush (fp);
  return FTPOK;
}
#endif /* ndef WPUT */
