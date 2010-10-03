/**
*  This file is part of rmlint.
*
*  rmlint is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, either version 3 of the License, or
*  (at your option) any later version.
*
*  rmlint is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with rmlint.  If not, see <http://www.gnu.org/licenses/>.
*
** Author: Christopher Pahl <sahib@online.de>:
** Hosted at the time of writing (Do 30. Sep 18:32:19 CEST 2010):
*  on http://github.com/sahib/rmlint
*   
**/

/* Needed for nftw() */ 
#define _XOPEN_SOURCE 500
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ftw.h>
#include <signal.h>
#include <regex.h>
#include <unistd.h>
#include <pthread.h>

#include "rmlint.h"
#include "filter.h"
#include "mode.h"
#include "defs.h"
#include "list.h"
#include "mt.h"

uint32 dircount = 0;
uint32 elems = 0;  
short iinterrupt = 0;
short tint = 0;  
pthread_attr_t p_attr; 

/**  
 * Callbock from signal() 
 * Counts number of CTRL-Cs 
 **/

static void interrupt(int p) 
{
	 /** This was connected with SIGINT (CTRL-C) in recurse_dir() **/ 
	 switch(tint)
	 {
		 case 0: warning(RED".. "GRE"aborting... "RED"@ \n"NCO); break; 
		 case 1: die(1);
	 }
	 iinterrupt++; 
	 tint++;  
}


/** 
 * grep the string "string" to see if it contains the pattern.
 * Will return 0 if yes, 1 otherwise.
 */ 

int regfilter(const char* input, const char *pattern)
{
  int status;
  int flags = REG_EXTENDED|REG_NOSUB; 
  
  
  const char *string = basename(input);
  
  if(!pattern||!string)
  {
	   return 0; 
  }
  else
  {
	  regex_t re;
	  
	  if(!set.casematch) 
		flags |= REG_ICASE; 
	  
	  if(regcomp(&re, pattern, flags)) 
		return 0; 

	  if( (status = regexec(&re, string, (size_t)0, NULL, 0)) != 0)
	  {
		  if(status != REG_NOMATCH)
		  {
			  char err_buff[100];
			  regerror(status, &re, err_buff, 100);
			  warning("Warning: Invalid regex pattern: '%s'\n", err_buff);
		  }
	  }
	  
	  regfree(&re);
	  return (set.invmatch) ? !(status) : (status);
  }
}

/** 
 * Callbock from nftw() 
 * If the file given from nftw by "path": 
 * - is a directory: recurs into it and catch the files there, 
 * 	as long the depth doesnt get bigger than max_depth and contains the pattern  cmp_pattern
 * - a file: Push it back to the list, if it has "cmp_pattern" in it. (if --regex was given) 
 * If the user interrupts, nftw() stops, and the program will build fingerprints.  
 **/ 
 
int eval_file(const char *path, const struct stat *ptr, int flag, struct FTW *ftwbuf)
{
	if(set.depth && ftwbuf->level > set.depth)
	{
		/* Do not recurse in this subdir */
		return FTW_SKIP_SIBLINGS; 
	} 
	if(iinterrupt)
	{
		return FTW_STOP;
	}
	if(flag == FTW_SLN)
	{
		error(RED"Bad symlink: %s\n"NCO,path);
		fprintf(get_logstream(),"rm %s #bad link\n",path); 
	}	
	if(ptr->st_size != 0) 
	{
		if(flag == FTW_F && ptr->st_rdev == 0)
		{
			if(!regfilter(path, set.fpattern))
			{
				dircount++; 
				list_append(path, ptr->st_size,ptr->st_dev,ptr->st_ino, ptr->st_nlink);
			}
			return FTW_CONTINUE; 
		}	
	}
	else
	{
		error(NCO"    Empty file: %s\n",path);
		fprintf(get_logstream(), "rm %s # empty file\n",path); 
	}
	if(flag == FTW_D)
	{	
		if(regfilter(path,set.dpattern)&& strcmp(path,set.paths[get_cpindex()]) != 0)
		{
			return FTW_SKIP_SUBTREE;
		}
	}
	return FTW_CONTINUE;
}



uint32 rem_double_paths(void)
{
	iFile *b = list_begin();
	iFile *s = NULL;  
	
	uint32 c = 0;
	while(b)
	{	
		s=list_begin(); 
		
		while(s)
		{
			if(s->dev == b->dev && s->node == b->node && b!=s)
			{
				c++; 
				s = list_remove(s); 
			}
			else
			{
				s=s->next; 
			}
		}
		b=b->next; 
	}
	
	return c;
}


void prefilter(void)
{
	iFile *b = list_begin();
	uint32 c =  0;
	uint32 l = list_getlen(); 

	iFile *s,*e; 

	if(b == NULL) die(0);
	
	while(b)
	{					
		if(iinterrupt)
		{
			iinterrupt = 0; 
			return; 
		}
	
		if(b->last && b->next)
		{
			if(b->last->fsize != b->fsize && b->next->fsize != b->fsize)
			{
				c++; 
				b = list_remove(b);
				continue;
			}
		}
		b=b->next; 
	}
	
	e=list_end(); 
	if(e->last)
	{
		if(e->fsize != e->last->fsize) 
		{
			e=list_remove(e);
		}
	}	
	
	s=list_begin(); 
	if(s->next) 
	{
		if(s->fsize != s->next->fsize) 
		{
			s=list_remove(s); 
		}
	}
	


	/* If we have more than 2 dirs given, or links may be followd
	 * we should check if the one is a subset of the another
	 * and remove path-doubles */
	if(get_cpindex() > 1 || set.followlinks) 
	{
		uint32 cb; 
		if( (cb=rem_double_paths()) )
			info(RED" => "NCO"Ignoring %ld pathdoubles.\n",cb);
	}
	if(c != 0)
	{
		info(RED" => "NCO"Prefiltered %ld of %ld files.                                        \n",c,l);
	}
}

static void read_sebytes(iFile *file)
{
	FILE *a = fopen(file->path,"rb"); 
	if(a) 
	{
		file->se[0]=fgetc(a);
		file->se[1]=fgetc(a);
		fseek(a,-2,SEEK_END);
		file->se[2]=fgetc(a); 
		file->se[3]=fgetc(a); 
		fclose(a); 
	}
}


static int cmp_sebytes(iFile *i, iFile *j) 
{
	if((i->se[0]==j->se[0])&&
	   (i->se[1]==j->se[1])&&
	   (i->se[2]==j->se[2])&&
	   (i->se[3]==j->se[3]) )
	    return 1;

	return 0; 
}


uint32 filter_template(void(*create)(iFile*),  int(*cmp)(iFile*,iFile*), const char *filter_name) 
{
	iFile *ptr = list_begin();
	iFile *sub = NULL; 
	
	uint32 con = 0;
	uint32 tol = list_getlen();  
	
	while(ptr)
	{
		iFile *i=ptr,*j=ptr; 
		uint32 fs = ptr->fsize;
		bool del = true; 
		 
		sub=ptr;
		 
		/** Get to **/
		while(ptr && ptr->fsize == fs) 
		{
			(*create)(ptr); 
			ptr=ptr->next; 
		}
		
		if(iinterrupt) 
		{
			iinterrupt = 0; 
			sub->fpc = 42;
			return 0; 
		}

		if(con % STATUS_UPDATE_INTERVAL == 0) 
		{
			info("Filtering by %s "BLU"["NCO"%ld"BLU"|"NCO"%ld"BLU"]"NCO"\r", filter_name ,con,tol); 
			fflush(stdout); 
		}
		while(i && i!=ptr)
		{
			j=sub; 
			del=true;
			
			while(j && j!=ptr)
			{
					if(i==j) 
					{
						j=j->next;
						continue; 
					}
					if((*cmp)(i,j))
					{
						del = false; 
						break; 
					}
					j = j->next;
			} 
			if(del)
			{
				i = list_remove(i); 
				con++; 
			}
			else
			{
				i=i->next; 
			}
		}
	}
	return con; 

}


static int cmp_fingerprints(iFile *a,iFile *b) 
{
	int i,j; 
	if(!(a&&b))
		return 0;
	
	for(i=0;i<2;i++) 
	{ 
		for(j=0;j<MD5_LEN;j++)  
		{
			if(a->fp[i][j] != b->fp[i][j])
			{
				return  0; 
			}
		}
	}
	return 1; 
}

/*** Calling filter_template() ***/

uint32 byte_filter(void) 
{
	return filter_template(read_sebytes,cmp_sebytes,"Bytecompare"); 
}


uint32 build_fingerprint(void)
{
	return filter_template(md5_fingerprint, cmp_fingerprints,"Fingerprint"); 
}

/** This function is pointless. **/
char blob(uint32 i)
{
	if(!i) return 'x';
	switch(i%12) 
	{
		case 0: return 'O'; 
		case 1: return '0';
		case 2: return 'o'; 
		case 3: return '*';
		case 4: return '|';
		case 5: return ':';
		case 6: return '.';
		case 7: return ' '; 
		case 8: return '-'; 
		case 9: return '|'; 
		case 10: return '^';
		case 11:return '0'; 
		default: return 'x'; 
	}
}


void build_checksums(void)
{


	uint32 c = 0; 
	float perc = 0; 
	iFile *ptr = list_begin(); 
		
	if(ptr == NULL) 
	{
		error(YEL" => "NCO"No files in the list after filtering..\n");
		error(YEL" => "NCO"This means that no duplicates were found. Exiting!\n"); 
		die(0);
	}
	

	while(ptr)
	{

		if(c%STATUS_UPDATE_INTERVAL==0)
		{
			/* Make the user happy with some progress */
			perc = (((float)c) / ((float)list_getlen())) * 100.0f; 
			info("Building checksums.. "GRE"%2.1f"BLU"%% "RED"["NCO"%ld"RED"/"NCO"%ld"RED"]"NCO" - ["BLU"%c"NCO"]  - ["RED"%ld"NCO" Bytes]      \r"NCO, perc,
							c, list_getlen(), blob(c) , ptr->fsize);
			fflush(stdout); 
		}
		
		c++;
		
		if(set.threads != 1)
		{
			/* Fill the threading pool with data
			 * If the pool is full then checksums are build 
			 * on $tt threads in parallel */
			fillpool(ptr,c);
			
		}
		else
		{	
			/* If only 1 thread is specified: 
			 * Run without the overhead of mt.c 
			 * and call the routines directly */
			MD5_CTX con;
			md5_file(ptr->path, &con);
			memcpy(ptr->md5_digest,con.digest, MD5_LEN); 
		}
		
		/* Neeeeext! */    
		ptr = ptr->next;
		
		/* The user told us that we have to hate him now. */
		if(iinterrupt)
		{
			 ptr->fpc = 42;
			 break;
		}
	}

}

int recurse_dir(const char *path)
{
  int flags = FTW_ACTIONRETVAL; 
  if(!set.followlinks) 
	flags |= FTW_PHYS;
	
  if(set.samepart)
	flags |= FTW_MOUNT;

  /* Handle SIGINT */
  signal(SIGINT, interrupt); 

  if( nftw(path, eval_file, _XOPEN_SOURCE, flags) == -1)
  {
    warning("nftw() failed with: %s\n", strerror(errno));
    return EXIT_FAILURE;
  } 

  return dircount; 
}