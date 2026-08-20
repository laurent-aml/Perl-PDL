#include "pdlcore.h"

#ifdef PDL_PTHREAD

/* We can only barf/warn from a thread that holds the interpreter, so a worker
 * pthread complains into a buffer and the thread that spawned it reports the lot
 * afterwards.  One of these holds those buffers.
 *
 * There is one per in-flight pdl_magic_thread_cast, plus one for an offloaded
 * transformation, and a worker reaches its own through thread-local storage.  That
 * is what lets a fan-out running on an offload backend's worker coexist with the
 * interpreter thread, which is free to run another transformation meanwhile:
 * each side complains to its own cast, and neither mistakes itself for the other.
 */
typedef struct pdl_pthread_ctx {
  char  *barf_msgs;
  size_t barf_msgs_len;
  char  *warn_msgs;
  size_t warn_msgs_len;
  char is_root;      /* an offloaded transformation, not a spawned worker: it may
                      * not pthread_exit, so it barfs the ordinary way */
  char defer_warns;  /* ... and it has no interpreter, so warnings travel home */
} pdl_pthread_ctx;

static pthread_key_t pdl_pthread_ctx_key;
static pthread_once_t pdl_pthread_ctx_once = PTHREAD_ONCE_INIT;

static void pdl_pthread_ctx_key_init(void) {
  pthread_key_create(&pdl_pthread_ctx_key, NULL);
}

static pdl_pthread_ctx *pdl_pthread_ctx_get(void) {
  pthread_once(&pdl_pthread_ctx_once, pdl_pthread_ctx_key_init);
  return (pdl_pthread_ctx *)pthread_getspecific(pdl_pthread_ctx_key);
}

static void pdl_pthread_ctx_set(pdl_pthread_ctx *ctx) {
  pthread_once(&pdl_pthread_ctx_once, pdl_pthread_ctx_key_init);
  pthread_setspecific(pdl_pthread_ctx_key, ctx);
}

/* Hand a finished cast's warnings to the enclosing offloaded transformation, the
 * only frame around that will be back on the interpreter thread to report them.
 * Both buffers are newline-separated and count their terminating '\0' in the
 * length, so the append writes over the first one's. */
static void pdl_pthread_ctx_take_warns(pdl_pthread_ctx *dst, pdl_pthread_ctx *src) {
  if (!src->warn_msgs_len) return;
  if (!dst->warn_msgs_len) {
    dst->warn_msgs = src->warn_msgs;
    dst->warn_msgs_len = src->warn_msgs_len;
  } else {
    char *p = realloc(dst->warn_msgs, dst->warn_msgs_len + src->warn_msgs_len - 1);
    if (p) {
      memcpy(p + dst->warn_msgs_len - 1, src->warn_msgs, src->warn_msgs_len);
      dst->warn_msgs = p;
      dst->warn_msgs_len += src->warn_msgs_len - 1;
    }
    free(src->warn_msgs);
  }
  src->warn_msgs = NULL;
  src->warn_msgs_len = 0;
}

#endif


/* Singly linked list */
/* Note that this zeroes ->next!) */

void pdl__magic_add(pdl *it,pdl_magic *mag)
{
        pdl_magic **foo = (pdl_magic **)(&(it->magic));
	while(*foo) {
		foo = &((*foo)->next);
	}
	(*foo) = mag;
	mag->next = NULL;
}

pdl_error pdl__magic_rm(pdl *it,pdl_magic *mag)
{
        pdl_error PDL_err = {0, NULL, 0};
        pdl_magic **foo = (pdl_magic **)(&(it->magic));
	int found = 0;
	while(*foo) {
		if(*foo == mag) {
			*foo = (*foo)->next;
			found = 1;
		}
		else{
			foo = &((*foo)->next);
		}
	}
	if( !found ){
		return pdl_make_error_simple(PDL_EUSERERROR, "PDL:Magic not found: Internal error\n");
	}
	return PDL_err;
}

void pdl__magic_free(pdl *it)
{
  if (pdl__ismagic(it) && !pdl__magic_isundestroyable(it)) {
    pdl_magic *foo = (pdl_magic *)(it->magic);
    while(foo) {
      pdl_magic *next = foo->next;
      free(foo);
      foo = next;
    }
  }
}

/* Test for undestroyability */

int pdl__magic_isundestroyable(pdl *it)
{
        pdl_magic **foo = (pdl_magic **)(&(it->magic));
	while(*foo) {
		if((*foo)->what & PDL_MAGIC_UNDESTROYABLE) {return 1;}
		foo = &((*foo)->next);
	}
	return 0;
}

/* Call magics */

void *pdl__call_magic(pdl *it,int which)
{
	void *ret = NULL;
	pdl_magic **foo = (pdl_magic **)(&(it->magic));
	while(*foo) {
		if((*foo)->what & which) {
			if((*foo)->what & PDL_MAGIC_DELAYED)
				pdl_add_delayed_magic(*foo);
			else
				ret = (*foo)->vtable->cast(*foo);
					/* Cast spell */
		}
		foo = &((*foo)->next);
	}
	return ret;
}

/* XXX FINDS ONLY FIRST */
pdl_magic *pdl__find_magic(pdl *it, int which)
{
        pdl_magic **foo = (pdl_magic **)(&(it->magic));
	while(*foo) {
		if((*foo)->what & which) {
			return *foo;
		}
		foo = &((*foo)->next);
	}
	return NULL;
}

pdl_magic *pdl__print_magic(pdl *it)
{
        pdl_magic **foo = (pdl_magic **)(&(it->magic));
	while(*foo) {
	  printf("Magic %p\ttype: ",*foo);
		if((*foo)->what & PDL_MAGIC_MARKCHANGED)
		  printf("PDL_MAGIC_MARKCHANGED");
		else if ((*foo)->what & PDL_MAGIC_THREADING)
		  printf("PDL_MAGIC_THREADING");
		else
		  printf("UNKNOWN");
		if ((*foo)->what & (PDL_MAGIC_DELAYED|PDL_MAGIC_UNDESTROYABLE))
		  {
		    printf(" qualifier(s): ");
		    if ((*foo)->what & PDL_MAGIC_DELAYED)
		      printf(" PDL_MAGIC_DELAYED");
		    if ((*foo)->what & PDL_MAGIC_UNDESTROYABLE)
		      printf(" PDL_MAGIC_UNDESTROYABLE");
		  }
		printf("\n");
		foo = &((*foo)->next);
	}
	return NULL;
}


int pdl__ismagic(pdl *it)
{
	return (it->magic != 0);
}

static pdl_magic **delayed=NULL;
static PDL_Indx ndelayed = 0;
void pdl_add_delayed_magic(pdl_magic *mag) {
    /* FIXME: Common realloc mistake: 'delayed' nulled but not freed upon failure */
	delayed = realloc(delayed,sizeof(*delayed)*++ndelayed);
	delayed[ndelayed-1] = mag;
}
void pdl_run_delayed_magic(void) {
	PDL_Indx i;
	pdl_magic **oldd = delayed; /* In case someone makes new delayed stuff */
	PDL_Indx nold = ndelayed;
	delayed = NULL;
	ndelayed = 0;
	for(i=0; i<nold; i++) {
		oldd[i]->vtable->cast(oldd[i]);
	}
	free(oldd);
}

/****************
 *
 * ->bind - magic
 */
void *svmagic_cast(pdl_magic *mag)
{
	pdl_magic_perlfunc *magp = (pdl_magic_perlfunc *)mag;
	dSP;
	ENTER; SAVETMPS;
	PUSHMARK(SP);
	perl_call_sv(magp->sv, G_DISCARD | G_NOARGS);
	FREETMPS; LEAVE;
	return NULL;
}

static pdl_magic_vtable svmagic_vtable = {
	svmagic_cast,
	NULL
};

pdl_magic *pdl_add_svmagic(pdl *it,SV *func)
{
	AV *av;
	pdl_magic_perlfunc *ptr = malloc(sizeof(pdl_magic_perlfunc));
	if (!ptr) return NULL;
	ptr->what = PDL_MAGIC_MARKCHANGED | PDL_MAGIC_DELAYED;
	ptr->vtable = &svmagic_vtable;
	ptr->sv = newSVsv(func);
	ptr->pdl = it;
	ptr->next = NULL;
	pdl__magic_add(it,(pdl_magic *)ptr);
	if(it->state & PDL_ANYCHANGED)
		pdl_add_delayed_magic((pdl_magic *)ptr);
/* In order to have our SV destroyed in time for the interpreter, */
/* XXX Work this out not to memleak */
	av = perl_get_av("PDL::disposable_svmagics",TRUE);
	av_push(av,ptr->sv);
	return (pdl_magic *)ptr;
}

#ifdef PDL_PTHREAD

/**************
 *
 * pthreads
 *
 */

typedef struct ptarg {
	pdl_magic_pthread *mag;
	pdl_error (*func)(pdl_trans *);
	pdl_trans *t;
	int no;
	pdl_error error_return;
	pdl_pthread_ctx *ctx;
} ptarg;

int pdl_pthreads_enabled(void) {return 1;}


static void *pthread_perform(void *vp) {
	struct ptarg *p = (ptarg *)vp;
	PDLDEBUG_f(printf("STARTING THREAD %d (%lu)\n",p->no, (long unsigned)pthread_self()));
	pthread_setspecific(p->mag->key,(void *)&(p->no));
	pdl_pthread_ctx_set(p->ctx);
	int oldtype; /* don't care but must supply */
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, &oldtype);
	p->error_return = (p->func)(p->t);
	PDLDEBUG_f(printf("ENDING THREAD %d (%lu)\n",p->no, (long unsigned)pthread_self()));
	return NULL;
}

int pdl_magic_thread_nthreads(pdl *it, PDL_Indx *nthdim) {
	pdl_magic_pthread *ptr = (pdl_magic_pthread *)pdl__find_magic(it, PDL_MAGIC_THREADING);
	if(!ptr) return 0;
	if (nthdim) *nthdim = ptr->nthdim;
	return ptr->nthreads;
}

int pdl_magic_get_thread(pdl *it) {
	pdl_magic_pthread *ptr = (pdl_magic_pthread *)pdl__find_magic(it, PDL_MAGIC_THREADING);
	if(!ptr) return -1;
	int *p = (int*)pthread_getspecific(ptr->key);
	if(!p) return -1;
	return *p;
}

pdl_error pdl_magic_thread_cast(pdl *it,pdl_error (*func)(pdl_trans *),pdl_trans *t, pdl_broadcast *broadcast) {
	pdl_error PDL_err = {0, NULL, 0};
	PDL_BRC_CHKMAGIC(broadcast);
	int clearMagic = 0; /* Flag = 1 if we are temporarily creating pthreading magic in the
						   supplied pdl.  */
	pdl_magic_pthread *ptr = (pdl_magic_pthread *)pdl__find_magic(it, PDL_MAGIC_THREADING);
	if(!ptr) {
		/* Magic doesn't exist, create it
			Probably was deleted before the transformation performed, due to
			pdl lazy evaluation.
		*/

		PDL_RETERROR(PDL_err, pdl_add_threading_magic(it, broadcast->mag_nth, broadcast->mag_nthr));
		clearMagic = 1; /* Set flag to delete magic later */

		/* Try to get magic again */
		ptr = (pdl_magic_pthread *)pdl__find_magic(it, PDL_MAGIC_THREADING);

		if(!ptr) {return pdl_make_error_simple(PDL_EFATAL, "Invalid pdl_magic_thread_cast!");}

	}

	pthread_t tp[broadcast->mag_nthr];
	ptarg tparg[broadcast->mag_nthr];
	pthread_key_create(&(ptr->key),NULL);
	/* Where the pthreads we are about to spawn will leave anything they have to
	 * say; they find it through TLS, and we report it once they have joined. */
	pdl_pthread_ctx ctx = {NULL, 0, NULL, 0, 0, 0};
	/* Set if we are ourselves running inside an offloaded transformation, in which
	 * case there is no interpreter here to warn with. */
	pdl_pthread_ctx *outer = pdl_pthread_ctx_get();

	PDLDEBUG_f(printf("CREATING THREADS, ME: TBD, key: %ld\n", (unsigned long)(ptr->key)));
	PDL_Indx i, last_pthread = -1;
	for(i=0; i<broadcast->mag_nthr; i++) {
	    tparg[i].mag = ptr;
	    tparg[i].func = func;
	    tparg[i].t = t;
	    tparg[i].no = i;
	    tparg[i].error_return = PDL_err;
	    tparg[i].ctx = &ctx;
	    if (pthread_create(tp+i, NULL, pthread_perform, tparg+i))
	      break;
	    last_pthread = i;
	}
	if (last_pthread < broadcast->mag_nthr-1) {
	  /* went wrong, cancel ones that started */
	  for (i=0; i <= last_pthread; i++)
	    pthread_cancel(tp[i]);
	}

	PDLDEBUG_f(printf("JOINING THREADS, ME: TBD, key: %ld\n", (unsigned long)(ptr->key)));
	for(i=0; i<=last_pthread; i++) {
		pthread_join(tp[i], NULL);
	}
	PDLDEBUG_f(printf("FINISHED THREADS, ME: TBD, key: %ld\n", (unsigned long)(ptr->key)));

	pthread_key_delete((ptr->key));

	/* Remove pthread magic if we created in this function */
	if( clearMagic ){
		PDL_RETERROR(PDL_err, pdl_add_threading_magic(it, -1, -1));
	}

#define handle_deferred_errors(type, action)							\
	do{															\
		if(ctx.type##_msgs_len != 0)							\
		{														\
			ctx.type##_msgs_len = 0;							\
			action;	\
			free(ctx.type##_msgs);								\
			ctx.type##_msgs	  = NULL;							\
		}														\
	} while(0)

	/* Warning means calling perl, which needs the interpreter: if we are running
	 * inside an offloaded transformation it is not ours to call, so the messages go
	 * out with the offload instead and are reported when it lands. */
	if (outer && outer->defer_warns)
	  pdl_pthread_ctx_take_warns(outer, &ctx);
	else
	  handle_deferred_errors(warn, pdl_pdl_warn("%s", ctx.warn_msgs));
	if (last_pthread < broadcast->mag_nthr-1)
	  return pdl_make_error_simple(PDL_EFATAL, "Failed to create at least one thread, aborting");
	handle_deferred_errors(barf, PDL_err = pdl_error_accumulate(PDL_err, pdl_make_error(PDL_EUSERERROR, "%s", ctx.barf_msgs)));
	for(i=0; i<broadcast->mag_nthr; i++) {
	    PDL_err = pdl_error_accumulate(PDL_err, tparg[i].error_return);
	}
	return PDL_err;
}

/* Function to remove threading magic (added by pdl_add_threading_magic) */
pdl_error pdl_rm_threading_magic(pdl *it)
{
	pdl_error PDL_err = {0, NULL, 0};
	pdl_magic_pthread *ptr = (pdl_magic_pthread *)pdl__find_magic(it, PDL_MAGIC_THREADING);
	/* Don't do anything if threading magic not found */
	if( !ptr) return PDL_err;
	/* Remove magic */
	PDL_RETERROR(PDL_err, pdl__magic_rm(it, (pdl_magic *) ptr));
	/* Free magic */
	free( ptr );
	return PDL_err;
}

/* Function to add threading magic (i.e. identify which PDL dimension should
   be pthreaded and how many pthreads to create
   Note: If nthdim and nthreads = -1 then any pthreading magic is removed */
pdl_error pdl_add_threading_magic(pdl *it,PDL_Indx nthdim,PDL_Indx nthreads)
{
	pdl_error PDL_err = {0, NULL, 0};
	pdl_magic_pthread *ptr;
	/* Remove threading magic if called with parms -1, -1 */
	if( (nthdim == -1) && ( nthreads == -1 ) ){
		 PDL_RETERROR(PDL_err, pdl_rm_threading_magic(it));
		 return PDL_err;
	}
	ptr = malloc(sizeof(pdl_magic_pthread));
	if (!ptr) return pdl_make_error_simple(PDL_EFATAL, "Out of memory");
	ptr->what = PDL_MAGIC_THREADING;
	ptr->vtable = NULL;
	ptr->next = NULL;
	ptr->nthdim = nthdim;
	ptr->nthreads = nthreads;
	pdl__magic_add(it,(pdl_magic *)ptr);
	return PDL_err;
}

/* An offloaded transformation runs on a backend's worker thread, which has no
 * interpreter.  Installing a context there gives a pthread fan-out inside it
 * somewhere to leave its warnings, and pdl_offload_ctx_flush reports them once the
 * handshake has put us back on the interpreter thread.
 *
 * The context is static, and so this is only good for one offloaded
 * transformation at a time; pdlapi.c enforces that before offloading.  Static
 * because install() runs on the worker, where allocating would mean calling into
 * perl - the very thing offloading exists to avoid.
 */
static pdl_pthread_ctx pdl_offload_ctx;

void pdl_offload_ctx_install(void) {
  pdl_offload_ctx = (pdl_pthread_ctx){NULL, 0, NULL, 0, 1, 1};
  pdl_pthread_ctx_set(&pdl_offload_ctx);
}

void pdl_offload_ctx_uninstall(void) {
  pdl_pthread_ctx_set(NULL);
}

void pdl_offload_ctx_flush(void) {
  if (pdl_offload_ctx.warn_msgs_len) {
    pdl_offload_ctx.warn_msgs_len = 0;
    pdl_pdl_warn("%s", pdl_offload_ctx.warn_msgs);
    free(pdl_offload_ctx.warn_msgs);
    pdl_offload_ctx.warn_msgs = NULL;
  }
}

char pdl_pthread_main_thread(void) {
  pdl_pthread_ctx *ctx = pdl_pthread_ctx_get();
  /* An offloaded transformation counts as the main thread: it must not exit the
   * backend's worker, and it reports failures by returning a pdl_error. */
  return !ctx || ctx->is_root;
}

// Barf/warn function for deferred barf message handling during pthreading We
// can't barf/warn during pthreading, because perl-level code isn't
// threadsafe. This routine does nothing if we're in the main thread (allowing
// the caller to barf normally, since there are not threading issues then). If
// we're in a worker thread, this routine stores the message for main-thread
// reporting later
int pdl_pthread_barf_or_warn(const char* pat, int iswarn, va_list *args)
{
	char** msgs;
	size_t* len;

	/* Don't do anything if we are in the main pthread */
	pdl_pthread_ctx *ctx = pdl_pthread_ctx_get();
	if (!ctx || ctx->is_root) return 0;

	if(iswarn)
	{
		msgs = &ctx->warn_msgs;
		len = &ctx->warn_msgs_len;
	}
	else
	{
		msgs = &ctx->barf_msgs;
		len = &ctx->barf_msgs_len;
	}

	size_t extralen = vsnprintf(NULL, 0, pat, *args);
	// add the new complaint to the list
	pdl_pthread_realloc_vsnprintf(msgs, len, extralen, pat, args, 1);

	if(iswarn)
	{
		/* Return 1, indicating we have handled the warn messages */
		return(1);
	}

	/* Exit the current pthread. Since this was a barf call, and we should be halting execution */
	pthread_exit(NULL);
	return 0;
}

void pdl_pthread_realloc_vsnprintf(char **p, size_t *len, size_t extralen, const char *pat, va_list *args, char add_newline) {
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock( &mutex );
  /* (For windows, we first #undef realloc
     so that the system realloc function is used instead of the PerlMem_realloc
     macro. This currently works fine, though could conceivably require some
     tweaking in the future if it's found to cause any problem.) */
#ifdef WIN32
#undef realloc
#endif
  if (add_newline) extralen += 1;
  extralen += 1; /* +1 for '\0' at end */
  *p = realloc(*p, *len + extralen);
  vsnprintf(*p + *len, extralen, pat, *args);
  *len += extralen; /* update the length-so-far, includes '\0' */
  if (add_newline) (*p)[*len-2] = '\n';
  (*p)[*len-1] = '\0';
  pthread_mutex_unlock( &mutex );
}

void pdl_pthread_free(void *p) {
#ifdef WIN32 /* same reasons as above */
#undef free
#endif
  static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
  pthread_mutex_lock( &mutex );
  free(p);
  pthread_mutex_unlock( &mutex );
}

/* copied from git@github.com:git/git.git 2.34-ish thread-util.c */
/* changed GIT_WINDOWS_NATIVE to WIN32 */
#if defined(hpux) || defined(__hpux) || defined(_hpux)
#  include <sys/pstat.h>
#endif
/*
 * By doing this in two steps we can at least get
 * the function to be somewhat coherent, even
 * with this disgusting nest of #ifdefs.
 */
#ifndef _SC_NPROCESSORS_ONLN
#  ifdef _SC_NPROC_ONLN
#    define _SC_NPROCESSORS_ONLN _SC_NPROC_ONLN
#  elif defined _SC_CRAY_NCPU
#    define _SC_NPROCESSORS_ONLN _SC_CRAY_NCPU
#  endif
#endif
int pdl_online_cpus(void)
{
#ifdef WIN32
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	if ((int)info.dwNumberOfProcessors > 0)
		return (int)info.dwNumberOfProcessors;
#elif defined(hpux) || defined(__hpux) || defined(_hpux)
	struct pst_dynamic psd;
	if (!pstat_getdynamic(&psd, sizeof(psd), (size_t)1, 0))
		return (int)psd.psd_proc_cnt;
#elif defined(HAVE_BSD_SYSCTL) && defined(HW_NCPU)
	int mib[2];
	size_t len;
	int cpucount;
	mib[0] = CTL_HW;
#  ifdef HW_AVAILCPU
	mib[1] = HW_AVAILCPU;
	len = sizeof(cpucount);
	if (!sysctl(mib, 2, &cpucount, &len, NULL, 0))
		return cpucount;
#  endif /* HW_AVAILCPU */
	mib[1] = HW_NCPU;
	len = sizeof(cpucount);
	if (!sysctl(mib, 2, &cpucount, &len, NULL, 0))
		return cpucount;
#endif /* defined(HAVE_BSD_SYSCTL) && defined(HW_NCPU) */
#ifdef _SC_NPROCESSORS_ONLN
	long ncpus;
	if ((ncpus = (long)sysconf(_SC_NPROCESSORS_ONLN)) > 0)
		return (int)ncpus;
#endif
	return 1;
}

#else
/* Dummy versions */
pdl_error pdl_add_threading_magic(pdl *it,PDL_Indx nthdim,PDL_Indx nthreads) {pdl_error PDL_err = {0,NULL,0}; return PDL_err;}
char pdl_pthread_main_thread() { return 1; }
void pdl_offload_ctx_install(void) {}
void pdl_offload_ctx_uninstall(void) {}
void pdl_offload_ctx_flush(void) {}
int pdl_magic_get_thread(pdl *it) {return 0;}
pdl_error pdl_magic_thread_cast(pdl *it,pdl_error (*func)(pdl_trans *),pdl_trans *t, pdl_broadcast *broadcast) {pdl_error PDL_err = {0,NULL,0}; return PDL_err;}
int pdl_magic_thread_nthreads(pdl *it,PDL_Indx *nthdim) {return 0;}
int pdl_pthreads_enabled() {return 0;}
int pdl_pthread_barf_or_warn(const char* pat, int iswarn, va_list *args){ return 0;}
int pdl_online_cpus() {return 1;}
#endif

/***************************
 *
 * Delete magic
 *
 */

static void *delete_mmapped_cast(pdl_magic *mag)
{
	pdl_magic_deletedata *magp = (pdl_magic_deletedata *)mag;
	magp->func(magp->pdl, magp->param);
	return NULL;
}

struct pdl_magic_vtable deletedatamagic_vtable = {
	delete_mmapped_cast,
	NULL
};

pdl_error pdl_add_deletedata_magic(pdl *it, void (*func)(pdl *, Size_t param), Size_t param)
{
	pdl_error PDL_err = {0, NULL, 0};
	pdl_magic_deletedata *ptr = malloc(sizeof(pdl_magic_deletedata));
	if (!ptr) return pdl_make_error_simple(PDL_EFATAL, "Out of memory");
	ptr->what = PDL_MAGIC_DELETEDATA;
	ptr->vtable = &deletedatamagic_vtable;
	ptr->pdl = it;
	ptr->func = func;
	ptr->param = param;
	pdl__magic_add(it, (pdl_magic *)ptr);
	return PDL_err;
}
