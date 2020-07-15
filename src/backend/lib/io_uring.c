
+# Check whether --with-liburing was given.
+if test "${with_liburing+set}" = set; then :
+  withval=$with_liburing;
+  case $withval in
+    yes)
+      :
+      ;;
+    no)
+      :
+      ;;
+    *)
+      as_fn_error $? "no argument expected for --with-liburing option" "$LINENO" 5
+      ;;
+  esac
+
+else
+  with_liburing=yes
+
+fi
 
 
 
@@ -11795,6 +11817,59 @@ fi
 
 fi
 
+if test "$with_liburing" = yes; then
+  { $as_echo "$as_me:${as_lineno-$LINENO}: checking for io_uring_queue_init in -luring" >&5
+$as_echo_n "checking for io_uring_queue_init in -luring... " >&6; }
+if ${ac_cv_luring_init+:} false; then :
+  $as_echo_n "(cached) " >&6
+else
+  ac_check_lib_save_LIBS=$LIBS
+LIBS="-luring  $LIBS"
+cat confdefs.h - <<_ACEOF >conftest.$ac_ext
+/* end confdefs.h.  */
+
+/* Override any GCC internal prototype to avoid an error.
+   Use char because int might match the return type of a GCC
+   builtin and then its argument prototype would still apply.  */
+#ifdef __cplusplus
+extern "C"
+#endif
+char io_uring_queue_init ();
+int
+main ()
+{
+return io_uring_queue_init ();
+  ;
+  return 0;
+}
+_ACEOF
+if ac_fn_c_try_link "$LINENO"; then :
+  ac_cv_luring_init=yes
+else
+  ac_cv_luring_init=no
+fi
+rm -f core conftest.err conftest.$ac_objext \
+    conftest$ac_exeext conftest.$ac_ext
+LIBS=$ac_check_lib_save_LIBS
+fi
+{ $as_echo "$as_me:${as_lineno-$LINENO}: result: $ac_cv_luring_init" >&5
+$as_echo "$ac_cv_luring_init" >&6; }
+if test "x$ac_cv_luring_init" = xyes; then :
+  cat >>confdefs.h <<_ACEOF
+#define HAVE_LIBURING 1
+_ACEOF
+
+  LIBS="-luring $LIBS"
+
+else
+  as_fn_error $? "io uring library not found
+If you have liburing already installed, see config.log for details on the
+failure.  It is possible the compiler isn't looking in the proper directory.
+Use --without-liburing to disable ip uring support." "$LINENO" 5
+fi
+
+fi
+
 if test "$enable_spinlocks" = yes; then
 
 $as_echo "#define HAVE_SPINLOCKS 1" >>confdefs.h
diff --git a/contrib/pg_prewarm/pg_prewarm.c b/contrib/pg_prewarm/pg_prewarm.c
index f3deb47a97..58f2dc02de 100644
--- a/contrib/pg_prewarm/pg_prewarm.c
+++ b/contrib/pg_prewarm/pg_prewarm.c
@@ -33,6 +33,7 @@ typedef enum
 {
 	PREWARM_PREFETCH,
 	PREWARM_READ,
+	PREWARM_ASYNC_READ,
 	PREWARM_BUFFER
 } PrewarmType;
 
@@ -84,6 +85,8 @@ pg_prewarm(PG_FUNCTION_ARGS)
 		ptype = PREWARM_PREFETCH;
 	else if (strcmp(ttype, "read") == 0)
 		ptype = PREWARM_READ;
+	else if (strcmp(ttype, "asyncread") == 0)
+		ptype = PREWARM_ASYNC_READ;
 	else if (strcmp(ttype, "buffer") == 0)
 		ptype = PREWARM_BUFFER;
 	else
@@ -182,6 +185,42 @@ pg_prewarm(PG_FUNCTION_ARGS)
 			++blocks_done;
 		}
 	}
+	else if (ptype == PREWARM_ASYNC_READ)
+	{
+#ifdef HAVE_LIBURING
+		int chunk = 0, chunk_size = async_queue_depth - 1;
+		int64 start = 0, stop = 0;
+
+		while (stop <= last_block)
+		{
+			start = first_block + chunk * chunk_size;
+			stop = start + chunk_size;
+
+			for (block = start; block <= stop; ++block)
+			{
+				CHECK_FOR_INTERRUPTS();
+				smgrqueueread(rel->rd_smgr, forkNumber, block, blockbuffer.data);
+			}
+
+			smgrsubmitread(rel->rd_smgr, forkNumber, block);
+
+			for (block = start; block <= stop; ++block)
+			{
+				BlockNumber readBlock;
+
+				CHECK_FOR_INTERRUPTS();
+				readBlock = smgrwaitread(rel->rd_smgr, forkNumber, block);
+				++blocks_done;
+			}
+
+			chunk++;
+		}
+#else
+		ereport(ERROR,
+				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
+				 errmsg("async read is not supported by this build")));
+#endif
+	}
 	else if (ptype == PREWARM_BUFFER)
 	{
 		/*
diff --git a/src/backend/storage/file/fd.c b/src/backend/storage/file/fd.c
index a76112d6cd..859e6039e7 100644
--- a/src/backend/storage/file/fd.c
+++ b/src/backend/storage/file/fd.c
@@ -79,6 +79,10 @@
 #include <sys/resource.h>		/* for getrlimit */
 #endif
 
+#ifdef HAVE_LIBURING
+#include "liburing.h"
+#endif
+
 #include "miscadmin.h"
 #include "access/xact.h"
 #include "access/xlog.h"
@@ -101,6 +105,9 @@
 #define PG_FLUSH_DATA_WORKS 1
 #endif
 
+
+int			async_queue_depth = 64;
+
 /*
 static int	numTempTableSpaces = -1;
 static int	nextTempTableSpace = 0;
 
+#ifdef HAVE_LIBURING
+struct io_uring 	ring;
+#endif
 
 /*--------------------
 
 	/* register proc-exit hook to ensure temp files are dropped at exit */
 	on_proc_exit(AtProcExit_Files, 0);
+
+#ifdef HAVE_LIBURING
+	int returnCode = io_uring_queue_init(async_queue_depth, &ring, 0);
+	if (returnCode < 0)
+		ereport(FATAL,
+				(errcode(ERRCODE_SYSTEM_ERROR),
+				 errmsg("Cannot init io uring async_queue_depth %d, %s",
+					    async_queue_depth, strerror(-returnCode))));
+#endif
 }
 
 /*
@@ -1912,6 +1931,96 @@ retry:
 	return returnCode;
 }
 
+int
+FileQueueRead(File file, char *buffer, int amount, off_t offset, uint32 id)
+{
+#ifdef HAVE_LIBURING
+	int				returnCode;
+	io_data		   *data;
+	struct io_uring_sqe *sqe;
+
+	Vfd		   *vfdP;
+
+	Assert(FileIsValid(file));
+
+	DO_DB(elog(LOG, "FileQueueRead: %d (%s) " INT64_FORMAT " %d %p",
+			   file, VfdCache[file].fileName,
+			   (int64) offset,
+			   amount, buffer));
+
+	returnCode = FileAccess(file);
+	if (returnCode < 0)
+		return returnCode;
+
+	vfdP = &VfdCache[file];
+
+	data = (io_data *) palloc(sizeof(io_data));
+	data->id = id;
+	data->ioVector.iov_base = buffer;
+	data->ioVector.iov_len = amount;
+
+	sqe = io_uring_get_sqe(&ring);
+	if (sqe != NULL)
+	{
+		io_uring_prep_readv(sqe, vfdP->fd, &data->ioVector, 1, offset);
+		io_uring_sqe_set_data(sqe, data);
+
+		return 0;
+	}
+	else
+	{
+		ereport(FATAL,
+				(errcode(ERRCODE_SYSTEM_ERROR),
+				 errmsg("Cannot get sqe, %s", strerror(-returnCode))));
+	}
+#else
+	ereport(ERROR,
+			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
+			 errmsg("async read is not supported")));
+#endif
+}
+
+int
+FileSubmitRead()
+{
+#ifdef HAVE_LIBURING
+	int			returnCode;
+	returnCode = io_uring_submit(&ring);
+	if (returnCode < 0)
+		return returnCode;
+
+	return 0;
+#else
+	ereport(ERROR,
+			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
+			 errmsg("async read is not supported")));
+#endif
+}
+
+io_data *
+FileWaitRead()
+{
+#ifdef HAVE_LIBURING
+	int			returnCode;
+	struct io_uring_cqe *cqe = NULL;
+
+	returnCode = io_uring_wait_cqe(&ring, &cqe);
+	if (returnCode < 0)
+	{
+		io_data	*data = (io_data *) palloc(sizeof(io_data));
+		data->returnCode = returnCode;
+		return data;
+	}
+
+	io_uring_cqe_seen(&ring, cqe);
+	return io_uring_cqe_get_data(cqe);
+#else
+	ereport(ERROR,
+			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
+			 errmsg("async read is not supported")));
+#endif
+}
+
 int
 FileWrite(File file, char *buffer, int amount, off_t offset,
 		  uint32 wait_event_info)
@@ -2797,6 +2906,10 @@ static void
 AtProcExit_Files(int code, Datum arg)
 {
 	CleanupTempFiles(false, true);
+
+#ifdef HAVE_LIBURING
+	io_uring_queue_exit(&ring);
+#endif
 }
 
 /*
diff --git a/src/backend/storage/smgr/md.c b/src/backend/storage/smgr/md.c
index 07f3c93d3f..1b988e051c 100644
--- a/src/backend/storage/smgr/md.c
+++ b/src/backend/storage/smgr/md.c
@@ -663,6 +663,70 @@ mdread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
 	}
 }
 
+/*
+ *	mdqueueread() -- Queue a read for the specified block from a relation.
+ */
+void
+mdqueueread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
+	   char *buffer)
+{
+	off_t		seekpos;
+	MdfdVec    *v;
+
+	v = _mdfd_getseg(reln, forknum, blocknum, false,
+					 EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);
+
+	seekpos = (off_t) BLCKSZ * (blocknum % ((BlockNumber) RELSEG_SIZE));
+
+	Assert(seekpos < (off_t) BLCKSZ * RELSEG_SIZE);
+
+	if (FileQueueRead(v->mdfd_vfd, buffer, BLCKSZ, seekpos, blocknum) < 0)
+		ereport(ERROR,
+				(errcode_for_file_access(),
+				 errmsg("could not queue read for block %u in file \"%s\": %m",
+						blocknum, FilePathName(v->mdfd_vfd))));
+}
+
+/*
+ *	mdsubmitread() -- Submit all queued reads for the specified block from a
+ *	relation.
+ */
+void
+mdsubmitread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
+{
+	MdfdVec    *v;
+	v = _mdfd_getseg(reln, forknum, blocknum, false,
+					 EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);
+
+	if (FileSubmitRead() < 0)
+		ereport(ERROR,
+				(errcode_for_file_access(),
+				 errmsg("could not submit reads for block %u in file \"%s\": %m",
+						blocknum, FilePathName(v->mdfd_vfd))));
+}
+
+/*
+ *	mdwaitread() -- Wait completion of a queued read for the specified block
+ *	from a relation.
+ */
+BlockNumber
+mdwaitread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
+{
+	MdfdVec    *v;
+	io_data    *data;
+	v = _mdfd_getseg(reln, forknum, blocknum, false,
+					 EXTENSION_FAIL | EXTENSION_CREATE_RECOVERY);
+
+	data = FileWaitRead();
+	if (data->returnCode < 0)
+		ereport(ERROR,
+				(errcode_for_file_access(),
+				 errmsg("could not wait read for block %u in file \"%s\": %m",
+						blocknum, FilePathName(v->mdfd_vfd))));
+	else
+		return (BlockNumber) data->id;
+}
+
 /*
  *	mdwrite() -- Write the supplied block at the appropriate location.
  *
diff --git a/src/backend/storage/smgr/smgr.c b/src/backend/storage/smgr/smgr.c
index b0d9f21e68..6d73d30db4 100644
--- a/src/backend/storage/smgr/smgr.c
+++ b/src/backend/storage/smgr/smgr.c
@@ -53,6 +53,12 @@ typedef struct f_smgr
 								  BlockNumber blocknum);
 	void		(*smgr_read) (SMgrRelation reln, ForkNumber forknum,
 							  BlockNumber blocknum, char *buffer);
+	void		(*smgr_queue_read) (SMgrRelation reln, ForkNumber forknum,
+							  BlockNumber blocknum, char *buffer);
+	void		(*smgr_submit_read) (SMgrRelation reln, ForkNumber forknum,
+							  BlockNumber blocknum);
+	BlockNumber	(*smgr_wait_read) (SMgrRelation reln, ForkNumber forknum,
+							  BlockNumber blocknum);
 	void		(*smgr_write) (SMgrRelation reln, ForkNumber forknum,
 							   BlockNumber blocknum, char *buffer, bool skipFsync);
 	void		(*smgr_writeback) (SMgrRelation reln, ForkNumber forknum,
@@ -76,6 +82,9 @@ static const f_smgr smgrsw[] = {
 		.smgr_extend = mdextend,
 		.smgr_prefetch = mdprefetch,
 		.smgr_read = mdread,
+		.smgr_queue_read = mdqueueread,
+		.smgr_submit_read = mdsubmitread,
+		.smgr_wait_read = mdwaitread,
 		.smgr_write = mdwrite,
 		.smgr_writeback = mdwriteback,
 		.smgr_nblocks = mdnblocks,
@@ -565,6 +574,37 @@ smgrread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
 	smgrsw[reln->smgr_which].smgr_read(reln, forknum, blocknum, buffer);
 }
 
+/*
+ *	smgrqueueread() -- queue a read for a particular block from a relation into
+ *					   the supplied buffer.
+ */
+void
+smgrqueueread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
+		 char *buffer)
+{
+	smgrsw[reln->smgr_which].smgr_queue_read(reln, forknum, blocknum, buffer);
+}
+
+/*
+ *	smgrsubmitread() -- submit all reads for a particular block from a relation
+ *						into the supplied buffer.
+ */
+void
+smgrsubmitread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
+{
+	smgrsw[reln->smgr_which].smgr_submit_read(reln, forknum, blocknum);
+}
+
+/*
+ *	smgrwaitread() -- wait a reads for a particular block from a relation into
+ *					  the supplied buffer.
+ */
+BlockNumber
+smgrwaitread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum)
+{
+	return smgrsw[reln->smgr_which].smgr_wait_read(reln, forknum, blocknum);
+}
+
 /*
  *	smgrwrite() -- Write the supplied buffer out.
  *
diff --git a/src/backend/utils/misc/guc.c b/src/backend/utils/misc/guc.c
index 90ffd89339..956d6cfc02 100644
--- a/src/backend/utils/misc/guc.c
+++ b/src/backend/utils/misc/guc.c
@@ -2338,6 +2338,16 @@ static struct config_int ConfigureNamesInt[] =
 		NULL, NULL, NULL
 	},
 
+	{
+		{"async_queue_depth", PGC_POSTMASTER, RESOURCES_KERNEL,
+			gettext_noop("Queue depth"),
+			NULL
+		},
+		&async_queue_depth,
+		64, 25, INT_MAX,
+		NULL, NULL, NULL
+	},
+
 	/*
 	 * See also CheckRequiredParameterValues() if this parameter changes
 	 */
diff --git a/src/include/pg_config.h.in b/src/include/pg_config.h.in
index 512213aa32..21c682f8f0 100644
--- a/src/include/pg_config.h.in
+++ b/src/include/pg_config.h.in
@@ -377,6 +377,9 @@
 /* Define to 1 if you have the `z' library (-lz). */
 #undef HAVE_LIBZ
 
+/* Define to 1 if you have the `uring' library (-luring). */
+#undef HAVE_LIBURING
+
 /* Define to 1 if the system has the type `locale_t'. */
 #undef HAVE_LOCALE_T
 
diff --git a/src/include/storage/fd.h b/src/include/storage/fd.h
index d2a8c52044..dcc9336f10 100644
--- a/src/include/storage/fd.h
+++ b/src/include/storage/fd.h
@@ -47,6 +47,7 @@ typedef int File;
 
 /* GUC parameter */
 extern PGDLLIMPORT int max_files_per_process;
+extern PGDLLIMPORT int async_queue_depth;
 extern PGDLLIMPORT bool data_sync_retry;
 
 /*
@@ -67,6 +68,13 @@ extern int	max_safe_fds;
 #define FILE_POSSIBLY_DELETED(err)	((err) == ENOENT || (err) == EACCES)
 #endif
 
+typedef struct io_data
+{
+	struct iovec 	 ioVector;
+	uint32		 	 id;
+	int				 returnCode;
+} io_data;
+
 /*
  * prototypes for functions in fd.c
  */
@@ -78,6 +86,10 @@ extern File OpenTemporaryFile(bool interXact);
 extern void FileClose(File file);
 extern int	FilePrefetch(File file, off_t offset, int amount, uint32 wait_event_info);
 extern int	FileRead(File file, char *buffer, int amount, off_t offset, uint32 wait_event_info);
+extern int	FileQueueRead(File file, char *buffer, int amount, off_t offset,
+						  uint32 id);
+extern int	FileSubmitRead();
+extern io_data *FileWaitRead();
 extern int	FileWrite(File file, char *buffer, int amount, off_t offset, uint32 wait_event_info);
 extern int	FileSync(File file, uint32 wait_event_info);
 extern off_t FileSize(File file);
diff --git a/src/include/storage/md.h b/src/include/storage/md.h
index c0f05e23ff..1048853232 100644
--- a/src/include/storage/md.h
+++ b/src/include/storage/md.h
@@ -32,6 +32,10 @@ extern void mdprefetch(SMgrRelation reln, ForkNumber forknum,
 					   BlockNumber blocknum);
 extern void mdread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
 				   char *buffer);
+extern void mdqueueread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum,
+				   char *buffer);
+extern void mdsubmitread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum);
+extern BlockNumber mdwaitread(SMgrRelation reln, ForkNumber forknum, BlockNumber blocknum);
 extern void mdwrite(SMgrRelation reln, ForkNumber forknum,
 					BlockNumber blocknum, char *buffer, bool skipFsync);
 extern void mdwriteback(SMgrRelation reln, ForkNumber forknum,
diff --git a/src/include/storage/smgr.h b/src/include/storage/smgr.h
index d286c8c7b1..0e51f26460 100644
--- a/src/include/storage/smgr.h
+++ b/src/include/storage/smgr.h
@@ -97,6 +97,12 @@ extern void smgrprefetch(SMgrRelation reln, ForkNumber forknum,
 						 BlockNumber blocknum);
 extern void smgrread(SMgrRelation reln, ForkNumber forknum,
 					 BlockNumber blocknum, char *buffer);
+extern void smgrqueueread(SMgrRelation reln, ForkNumber forknum,
+					 BlockNumber blocknum, char *buffer);
+extern void smgrsubmitread(SMgrRelation reln, ForkNumber forknum,
+					 BlockNumber blocknum);
+extern BlockNumber smgrwaitread(SMgrRelation reln, ForkNumber forknum,
+					 BlockNumber blocknum);
 extern void smgrwrite(SMgrRelation reln, ForkNumber forknum,
 					  BlockNumber blocknum, char *buffer, bool skipFsync);
 extern void smgrwriteback(SMgrRelation reln, ForkNumber forknum,
-- 
2.21.0

