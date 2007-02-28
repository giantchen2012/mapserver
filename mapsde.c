/******************************************************************************
 *
 * Project:  MapServer
 * Purpose:  Implements SDE CONNECTIONTYPE.
 * Author:   Steve Lime and Howard Butler
 *
 ******************************************************************************
 * Copyright (c) 1996-2005 Regents of the University of Minnesota.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in 
 * all copies of this Software or works derived from this Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ******************************************************************************
 *
 * $Log$
 * Revision 1.113  2007/02/28 01:28:18  hobu
 * fix 2040
 *
 * Revision 1.112  2007/02/26 17:01:47  hobu
 * delete the SDE raster code for good.  This is now available in GDAL.
 *
 * Revision 1.111  2007/02/26 16:41:09  hobu
 * trim log
 *
 * Revision 1.110  2006/10/31 17:03:50  hobu
 * make sure to initialize sde->row_id_column to NULL or we'll blow
 * up on close for operations like GetCapabilities
 *
 * Revision 1.109  2006/08/11 16:58:02  dan
 * Added ability to encrypt tokens (passwords, etc.) in database connection
 * strings (MS-RFC-18, bug 1792)
 *

 *
*/

#include <time.h>
#include <assert.h>

#include "map.h"
#include "maperror.h"
#include "maptime.h"
#include "mapthread.h"



#ifdef USE_SDE
#include <sdetype.h> /* ESRI SDE Client Includes */
#include <sdeerno.h>

/*
#ifdef USE_SDERASTER
#include <sderaster.h>
#endif
*/

#define MS_SDE_MAXBLOBSIZE 1024*50 /* 50 kbytes */
#define MS_SDE_NULLSTRING "<null>"
#define MS_SDE_SHAPESTRING "<shape>"
#define MS_SDE_TIMEFMTSIZE 128 /* bytes */
#define MS_SDE_TIMEFMT "%T %m/%d/%Y"
#define MS_SDE_ROW_ID_COLUMN "SE_ROW_ID"

typedef struct {
  SE_CONNECTION connection;
  SE_STREAM stream;
} msSDEConnPoolInfo; 

typedef struct { 
  msSDEConnPoolInfo *connPoolInfo;
  SE_CONNECTION connection;
  SE_LAYERINFO layerinfo;
  SE_COORDREF coordref;
  SE_STREAM stream;
  long state_id;
  char *table, *column, *row_id_column;
} msSDELayerInfo;

typedef struct {
  long layerId;
  char *table;
  char *column;
  char *connection;
} layerId;

/*
 * Layer ID caching section.
 */
 
static int lcacheCount = 0;
static int lcacheMax = 0;
static layerId *lcache = NULL;



/************************************************************************/
/*       Start SDE/MapServer helper functions.                          */
/*                                                                      */
/************************************************************************/



/* -------------------------------------------------------------------- */
/* msSDECloseConnection                                                 */
/* -------------------------------------------------------------------- */
/*     Closes the SDE connection handle, which is given as a callback   */
/*     function to the connection pooling API                           */
/* -------------------------------------------------------------------- */
static void msSDECloseConnection( void *conn_handle )
{

  long status;
  msSDEConnPoolInfo *poolinfo = conn_handle;

  if (poolinfo) {
     if (poolinfo->stream) {
        SE_stream_free(poolinfo->stream);
     }
     if (poolinfo->connection) {
        status = SE_connection_free_all_locks (poolinfo->connection);
        if (status == SE_SUCCESS) {
           SE_connection_free(poolinfo->connection);
        }
     }
     free(poolinfo);
  } 

}

/* -------------------------------------------------------------------- */
/* sde_error                                                            */
/* -------------------------------------------------------------------- */
/*     Reports more detailed error information from SDE                 */
/* -------------------------------------------------------------------- */
static void sde_error(long error_code, char *routine, char *sde_routine) 
{
  char error_string[SE_MAX_MESSAGE_LENGTH];

  error_string[0] = '\0';
  SE_error_get_string(error_code, error_string);

  msSetError( MS_SDEERR, 
              "%s: %s. (%ld)", 
              routine, 
              sde_routine, 
              error_string, 
              error_code);

  return;
}

/* -------------------------------------------------------------------- */
/* msSDELayerGetRowIDColumn                                             */
/* -------------------------------------------------------------------- */
/*     A helper function to return unique row ID column for             */ 
/*     an opened SDE layer                                              */
/* -------------------------------------------------------------------- */
char *msSDELayerGetRowIDColumn(layerObj *layer)
{
#ifdef USE_SDE
  long status, column_type; 
  char* column_name;
  SE_REGINFO registration;

  msSDELayerInfo *sde=NULL;
  sde = layer->layerinfo;


  column_name = (char*) malloc(SE_MAX_COLUMN_LEN);
  if(!sde) {
    msSetError( MS_SDEERR, 
                "SDE layer has not been opened.", 
                "msSDELayerGetSpatialColumn()");
    return(NULL);
  }   
  
  if (sde->state_id == SE_DEFAULT_STATE_ID) {
    if(layer->debug) 
      msDebug("msSDELayerGetRowIDColumn(): State ID was SE_DEFAULT_STATE_ID, "
              "reverting to %s.\n", 
              MS_SDE_ROW_ID_COLUMN);
      return(strdup(MS_SDE_ROW_ID_COLUMN));
  }
  else 
  {
    status = SE_reginfo_create (&registration);
    if(status != SE_SUCCESS) {
      sde_error(status, "msSDELayerGetRowIDColumn()", "SE_reginfo_create()");
      return(NULL);
    }
    
    status = SE_registration_get_info ( sde->connection, 
                                        sde->table, 
                                        registration);
    if(status != SE_SUCCESS) {
      sde_error(status, 
                "msSDELayerGetRowIDColumn()", 
                "SE_registration_get_info()");
      return(NULL);
    }
    
    status= SE_reginfo_get_rowid_column ( registration, 
                                          column_name, 
                                          &column_type);
    SE_reginfo_free(registration);
    if(status != SE_SUCCESS) {
      sde_error(status, 
                "msSDELayerGetRowIDColumn()", 
                "SE_reginfo_get_rowid_column()");
      return(NULL);
    }
    if (column_type == SE_REGISTRATION_ROW_ID_COLUMN_TYPE_NONE){
      if(layer->debug) {
        msDebug("msSDELayerGetRowIDColumn(): Table was not registered, "
        "returning %s.\n", 
        MS_SDE_ROW_ID_COLUMN);
      }
      return (MS_SDE_ROW_ID_COLUMN);
    }
    
    if (column_name){

      return (column_name); 
    }
    else {
      free(column_name);
      return(strdup(MS_SDE_ROW_ID_COLUMN));
    }
}
#else
  msSetError( MS_MISCERR, 
              "SDE support is not available.", 
              "msSDELayerGetRowIDColumn()");
  return(NULL);
#endif
}


/* -------------------------------------------------------------------- */
/* msSDELCacheAdd                                                       */
/* -------------------------------------------------------------------- */
/*     Adds a SDE layer to the global layer cache.                      */
/* -------------------------------------------------------------------- */
long msSDELCacheAdd( layerObj *layer,
                     SE_LAYERINFO layerinfo,
                     char *tableName,
                     char *columnName,
                     char *connectionString) 
{
  
  layerId *lid = NULL;
  int status = 0;
  
  msAcquireLock( TLOCK_SDE );
  
  if (layer->debug){
    msDebug( "%s: Caching id for %s, %s, %s\n", "msSDELCacheAdd()", 
            tableName, columnName, connectionString);
  }
  /*
   * Ensure the cache is large enough to hold the new item.
   */
  if(lcacheCount == lcacheMax)
  {
    lcacheMax += 10;
    lcache = (layerId *)realloc(lcache, sizeof(layerId) * lcacheMax);
    if(lcache == NULL)
    {
      msReleaseLock( TLOCK_SDE );
      msSetError(MS_MEMERR, NULL, "msSDELCacheAdd()");
      return (MS_FAILURE);
    }
  }

  /*
   * Population the new lcache object.
   */
  lid = lcache + lcacheCount;
  lcacheCount++;

  status = SE_layerinfo_get_id(layerinfo, &lid->layerId);
  if(status != SE_SUCCESS)
  {
        msReleaseLock( TLOCK_SDE );
        sde_error(status, "msSDELCacheAdd()", "SE_layerinfo_get_id()");
        return(MS_FAILURE);
  }
  lid->table = strdup(tableName);
  lid->column = strdup(columnName);
  lid->connection = strdup(connectionString);
  
  msReleaseLock( TLOCK_SDE );
  return (MS_SUCCESS);
}

/* -------------------------------------------------------------------- */
/* msSDEGetLayerInfo                                                    */
/* -------------------------------------------------------------------- */
/*     Get a LayerInfo for the layer.  Cached layer is used if it       */
/*     exists in the cache.                                             */
/* -------------------------------------------------------------------- */
long msSDEGetLayerInfo(layerObj *layer,
                       SE_CONNECTION conn, 
                       char *tableName, 
                       char *columnName, 
                       char *connectionString,
                       SE_LAYERINFO layerinfo)
{
  int i;
  long status;
  layerId *lid = NULL;
  
  /*
   * If table or column are null, nothing can be done.
   */
  if(tableName == NULL)
  {
    msSetError( MS_MISCERR,
                "Missing table name.\n",
                "msSDEGetLayerInfo()");
    return (MS_FAILURE);
  }
  if(columnName == NULL)
  {
    msSetError( MS_MISCERR,
                "Missing column name.\n",
                "msSDEGetLayerInfo()");
    return (MS_FAILURE);
  }
  if(connectionString == NULL)
  {
    msSetError( MS_MISCERR,
                "Missing connection string.\n",
                "msSDEGetLayerInfo()");
    return (MS_FAILURE);
  }  

  if (layer->debug){
    msDebug("%s: Looking for layer by %s, %s, %s\n", "msSDEGetLayerInfo()",
          tableName, columnName, connectionString);
  }
  /*
   * Search the lcache for the layer id.
   */
  for(i = 0; i < lcacheCount; i++)
  {
    lid = lcache + i;
    if(strcasecmp(lid->table, tableName) == 0 &&
        strcasecmp(lid->column, columnName) == 0 &&
        strcasecmp(lid->connection, connectionString) == 0)
    {
      status = SE_layer_get_info_by_id(conn, lid->layerId, layerinfo);
      if(status != SE_SUCCESS) {
        sde_error(status, "msSDEGetLayerInfo()", "SE_layer_get_info()");
        return(MS_FAILURE);
      }
      else
      {
        if (layer->debug){
          msDebug( "%s: Matched layer to id %i.\n", 
                   "msSDEGetLayerId()", lid->layerId);
        }
        return (MS_SUCCESS);
      }
    }
  }
  if (layer->debug){
    msDebug("%s: No cached layerid found.\n", "msSDEGetLayerInfo()");
  }
  /*
   * No matches found, create one.
   */
  status = SE_layer_get_info( conn, tableName, columnName, layerinfo );
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDEGetLayerInfo()", "SE_layer_get_info()");
    return(MS_FAILURE);
  }
  else 
  {
    status = msSDELCacheAdd(layer, layerinfo, tableName, columnName, connectionString);
    return(MS_SUCCESS);
  }
}

/* -------------------------------------------------------------------- */
/* sdeShapeCopy                                                         */
/* -------------------------------------------------------------------- */
/*     Copies a SDE shape into a MapServer shapeObj                     */
/* -------------------------------------------------------------------- */
static int sdeShapeCopy(SE_SHAPE inshp, shapeObj *outshp) {

  SE_POINT *points=NULL;
  SE_ENVELOPE envelope;
  long type, status;
  long *part_offsets = NULL;
  long *subpart_offsets = NULL;
  long num_parts = -1;
  long num_subparts = -1;
  long num_points = -1;
  
  lineObj line={0,NULL};

  int i,j,k;

  status = SE_shape_get_type(inshp, &type);
  if(status != SE_SUCCESS) {
    sde_error(status, 
              "sdeCopyShape()", 
              "SE_shape_get_type()");
    return(MS_FAILURE);
  }
  
  switch(type) {
  case(SG_NIL_SHAPE):
    return(MS_SUCCESS); /* skip null shapes */
    break;
  case(SG_POINT_SHAPE):
  case(SG_MULTI_POINT_SHAPE):
    outshp->type = MS_SHAPE_POINT;
    break;
  case(SG_LINE_SHAPE):
  case(SG_SIMPLE_LINE_SHAPE): 
  case(SG_MULTI_LINE_SHAPE):
  case(SG_MULTI_SIMPLE_LINE_SHAPE):
    outshp->type = MS_SHAPE_LINE;
    break;
  case(SG_AREA_SHAPE):
  case(SG_MULTI_AREA_SHAPE):
    outshp->type = MS_SHAPE_POLYGON;
    break;  
  default:
    msSetError( MS_SDEERR, 
                "Unsupported SDE shape type (%ld).", 
                "sdeCopyShape()", 
                type);
    return(MS_FAILURE);
  }


  SE_shape_get_num_parts (inshp, &num_parts, &num_subparts);
  SE_shape_get_num_points (inshp, 0, 0, &num_points); 
	 
  part_offsets = (long *) malloc( (num_parts + 1) * sizeof(long));
  subpart_offsets = (long *) malloc( (num_subparts + 1)	* sizeof(long));
  part_offsets[num_parts] = num_subparts;
  subpart_offsets[num_subparts]	= num_points;

  points = (SE_POINT *)malloc(num_points*sizeof(SE_POINT));
  if(!points) {
    msSetError( MS_MEMERR, 
                "Unable to allocate points array.", 
                "sdeCopyShape()");
    return(MS_FAILURE);
  }

  status = SE_shape_get_all_points( inshp, 
                                    SE_DEFAULT_ROTATION, 
                                    part_offsets, 
                                    subpart_offsets, 
                                    points, 
                                    NULL, 
                                    NULL);
  if(status != SE_SUCCESS) {
    sde_error(status, "sdeCopyShape()", "SE_shape_get_all_points()");
    return(MS_FAILURE);
  }

  k = 0; /* overall point counter */
  for(i=0; i<num_subparts; i++) {
    
    if( i == num_subparts-1)
      line.numpoints = num_points - subpart_offsets[i];
    else
      line.numpoints = subpart_offsets[i+1] - subpart_offsets[i];

    line.point = (pointObj *)malloc(sizeof(pointObj)*line.numpoints);
    if(!line.point) {
      msSetError( MS_MEMERR, 
                  "Unable to allocate temporary point cache.", 
                  "sdeShapeCopy()");
      return(MS_FAILURE);
    }
     
    for(j=0; j < line.numpoints; j++) {
      line.point[j].x = points[k].x; 
      line.point[j].y = points[k].y;     
      k++;
    }

    msAddLine(outshp, &line);
    free(line.point);
  }

  free(part_offsets);
  free(subpart_offsets);
  free(points);

  /* finally copy the bounding box for the entire shape */
  SE_shape_get_extent(inshp, 0, &envelope);
  outshp->bounds.minx = envelope.minx;
  outshp->bounds.miny = envelope.miny;
  outshp->bounds.maxx = envelope.maxx;
  outshp->bounds.maxy = envelope.maxy;

  return(MS_SUCCESS);
}

/* -------------------------------------------------------------------- */
/* sdeGetRecord                                                         */
/* -------------------------------------------------------------------- */
/*     Retrieves the current row as setup via the SDE stream query      */
/*     or row fetch routines.                                           */
/* -------------------------------------------------------------------- */
static int sdeGetRecord(layerObj *layer, shapeObj *shape) {
  int i;
  long status;

  double doubleval;
  long longval;
  struct tm dateval;

  short shortval; /* new gdv */
  float floatval;

  SE_COLUMN_DEF *itemdefs;
  SE_SHAPE shapeval=0;
  SE_BLOB_INFO blobval;
 /* blobval = (SE_BLOB_INFO *) malloc(sizeof(SE_BLOB_INFO)); */
  msSDELayerInfo *sde;

  sde = layer->layerinfo;

  if(layer->numitems > 0) {
    shape->numvalues = layer->numitems;
    shape->values = (char **)malloc(sizeof(char *)*layer->numitems);
    if(!shape->values) {
      msSetError( MS_MEMERR, 
                  "Error allocation shape attribute array.", 
                  "sdeGetRecord()");
      return(MS_FAILURE);
    }
  }

  status = SE_shape_create(NULL, &shapeval);
  if(status != SE_SUCCESS) {
    sde_error(status, "sdeGetRecord()", "SE_shape_create()");
    return(MS_FAILURE);
  }

  itemdefs = layer->iteminfo;
  for(i=0; i<layer->numitems; i++) {

    if(strcmp(layer->items[i],sde->row_id_column) == 0) {
      status = SE_stream_get_integer(sde->stream, (short)(i+1), &shape->index);
      if(status != SE_SUCCESS) {
         sde_error(status, "sdeGetRecord()", "SE_stream_get_integer()");
         return(MS_FAILURE);
      }

      shape->values[i] = (char *)malloc(64); /* should be enough */
      sprintf(shape->values[i], "%ld", shape->index);
      continue;
    }    
    
    switch(itemdefs[i].sde_type) {
    case SE_SMALLINT_TYPE:
      /* changed by gdv */
      status = SE_stream_get_smallint(sde->stream, (short)(i+1), &shortval); 
      if(status == SE_SUCCESS)
        shape->values[i] = long2string(shortval);
      else if(status == SE_NULL_VALUE)
        shape->values[i] = strdup(MS_SDE_NULLSTRING);
      else {
        sde_error(status, "sdeGetRecord()", "SE_stream_get_smallint()");
        return(MS_FAILURE);
      }
      break;
    case SE_INTEGER_TYPE:
      status = SE_stream_get_integer(sde->stream, (short)(i+1), &longval);
      if(status == SE_SUCCESS)
        shape->values[i] = long2string(longval);
      else if(status == SE_NULL_VALUE)
        shape->values[i] = strdup(MS_SDE_NULLSTRING);
      else {
        sde_error(status, "sdeGetRecord()", "SE_stream_get_integer()");
        return(MS_FAILURE);
      }      
      break;
    case SE_FLOAT_TYPE:
      /* changed by gdv */
      status = SE_stream_get_float(sde->stream, (short)(i+1), &floatval); 
      if(status == SE_SUCCESS)
        shape->values[i] = double2string(floatval);
      else if(status == SE_NULL_VALUE)
        shape->values[i] = strdup(MS_SDE_NULLSTRING);
      else {     
        sde_error(status, "sdeGetRecord()", "SE_stream_get_float()");
        return(MS_FAILURE);
      }
      break;
    case SE_DOUBLE_TYPE:
      status = SE_stream_get_double(sde->stream, (short) (i+1), &doubleval);
      if(status == SE_SUCCESS)
        shape->values[i] = double2string(doubleval);
      else if(status == SE_NULL_VALUE)
        shape->values[i] = strdup(MS_SDE_NULLSTRING);
      else {     
        sde_error(status, "sdeGetRecord()", "SE_stream_get_double()");
        return(MS_FAILURE);
      }
      break;
    case SE_STRING_TYPE:
      shape->values[i] = (char *)malloc(itemdefs[i].size+1);
      status = SE_stream_get_string(sde->stream, 
                                    (short) (i+1), 
                                    shape->values[i]);
      if(status == SE_NULL_VALUE)
        shape->values[i][0] = '\0'; /* empty string */
      else if(status != SE_SUCCESS) {
        sde_error(status, "sdeGetRecord()", "SE_stream_get_string()");
        return(MS_FAILURE);
      }
      break;
    case SE_BLOB_TYPE:
        status = SE_stream_get_blob(sde->stream, (short) (i+1), &blobval);
        if(status == SE_SUCCESS) {
          shape->values[i] = (char *)malloc(sizeof(char)*blobval.blob_length);
          shape->values[i] = memcpy(shape->values[i],
                                    blobval.blob_buffer, 
                                    blobval.blob_length);
          SE_blob_free(&blobval);
        }
        else if (status == SE_NULL_VALUE) {
          shape->values[i] = strdup(MS_SDE_NULLSTRING);
        }
        else {
          sde_error(status, "sdeGetRecord()", "SE_stream_get_blob()");
          return(MS_FAILURE);
        }
      break;
    case SE_DATE_TYPE:
      status = SE_stream_get_date(sde->stream, (short)(i+1), &dateval);
      if(status == SE_SUCCESS) {
        shape->values[i] = (char *)malloc(sizeof(char)*MS_SDE_TIMEFMTSIZE);
        strftime( shape->values[i], 
                  MS_SDE_TIMEFMTSIZE, 
                  MS_SDE_TIMEFMT, 
                  &dateval);
      } else if(status == SE_NULL_VALUE)
        shape->values[i] = strdup(MS_SDE_NULLSTRING);
      else {     
        sde_error(status, "sdeGetRecord()", "SE_stream_get_date()");
        return(MS_FAILURE);
      }
      break;
    case SE_SHAPE_TYPE:
      status = SE_stream_get_shape(sde->stream, (short)(i+1), shapeval);
      if(status == SE_SUCCESS)
        shape->values[i] = strdup(MS_SDE_SHAPESTRING);
      else if(status == SE_NULL_VALUE)
        shape->values[i] = strdup(MS_SDE_NULLSTRING);
      else {
        sde_error(status, "sdeGetRecord()", "SE_stream_get_shape()");
        return(MS_FAILURE);
      }
      break;
    default: 
      msSetError(MS_SDEERR, "Unknown SDE column type.", "sdeGetRecord()");
      return(MS_FAILURE);
      break;
    }
  }

  if(SE_shape_is_nil(shapeval)) return(MS_SUCCESS);
  
  /* copy sde shape to a mapserver shape */
  status = sdeShapeCopy(shapeval, shape);
  if(status != MS_SUCCESS) return(MS_FAILURE);

  /* clean up */
  SE_shape_free(shapeval);

  
  return(MS_SUCCESS);
}
#endif

/************************************************************************/
/*       Start SDE/MapServer library functions.                         */
/*                                                                      */
/************************************************************************/

/* -------------------------------------------------------------------- */
/* msSDELayerOpen                                                       */
/* -------------------------------------------------------------------- */
/*     Connects to SDE using the SDE C API.  Connections are pooled     */
/*     using the MapServer pooling API.  After a connection is made,    */
/*     a query stream is created, using the SDE version specified in    */
/*     the DATA string, or SDE.DEFAULT if not specified.  It is         */
/*     important to note that the SE_CONNECTION is shared across data   */
/*     layers, but the state_id for a layer's version is different,     */
/*     even for layers with the same version name.  These are *not*     */
/*     shared across layers.                                            */
/* -------------------------------------------------------------------- */
int msSDELayerOpen(layerObj *layer) {
#ifdef USE_SDE
  long status=-1;
  char **params=NULL;
  char **data_params=NULL;
  int numparams=0;
  SE_ERROR error;
  SE_STATEINFO state;
  SE_VERSIONINFO version;

  msSDELayerInfo *sde;
  msSDEConnPoolInfo *poolinfo;


  /* layer already open, silently return */
  /* if(layer->layerinfo) return(MS_SUCCESS);  */

  /* allocate space for SDE structures */
  sde = (msSDELayerInfo *) malloc(sizeof(msSDELayerInfo));
  if(!sde) {
    msSetError( MS_MEMERR, 
                "Error allocating SDE layer structure.", 
                "msSDELayerOpen()");
    return(MS_FAILURE);
  }
 
  sde->state_id = SE_BASE_STATE_ID;
  
  /* initialize the table and spatial column names */
  sde->table = NULL;
  sde->column = NULL;
  sde->row_id_column = NULL;
  
  /* request a connection and stream from the pool */
  poolinfo = (msSDEConnPoolInfo *)msConnPoolRequest( layer ); 
  
  /* If we weren't returned a connection and stream, initialize new ones */
  if (!poolinfo) {
    char *conn_decrypted;

    if (layer->debug) 
      msDebug("msSDELayerOpen(): "
              "Layer %s opened from scratch.\n", layer->name);


    poolinfo = malloc(sizeof *poolinfo);
    if (!poolinfo) {
      return MS_FAILURE;
    } 		

    /* Decrypt any encrypted token in the connection string */
    conn_decrypted = msDecryptStringTokens(layer->map, layer->connection);
    if (conn_decrypted == NULL) {
        return(MS_FAILURE);  /* An error should already have been produced */
    }
    /* Split the connection parameters and make sure we have enough of them */
    params = split(conn_decrypted, ',', &numparams);
    if(!params) {
      msSetError( MS_MEMERR, 
                  "Error spliting SDE connection information.", 
                  "msSDELayerOpen()");
      msFree(conn_decrypted);
      return(MS_FAILURE);
    }
    msFree(conn_decrypted);
    conn_decrypted = NULL;

    if(numparams < 5) {
      msSetError( MS_SDEERR, 
                  "Not enough SDE connection parameters specified.", 
                  "msSDELayerOpen()");
      return(MS_FAILURE);
    }
  
    /* Create the connection handle and put into poolinfo->connection */
    status = SE_connection_create(params[0], 
                                  params[1], 
                                  params[2], 
                                  params[3], 
                                  params[4], 
                                  &error, 
                                  &(poolinfo->connection));

    if(status != SE_SUCCESS) {
      sde_error(status, "msSDELayerOpen()", "SE_connection_create()");
      return(MS_FAILURE);
    }

    /* ------------------------------------------------------------------------- */
    /* Set the concurrency type for the connection.  SE_UNPROTECTED_POLICY is    */
    /* suitable when only one thread accesses the specified connection.          */
    /* ------------------------------------------------------------------------- */
    status = SE_connection_set_concurrency( poolinfo->connection, 
                                            SE_UNPROTECTED_POLICY);

  
    if(status != SE_SUCCESS) {
      sde_error(status, "msSDELayerOpen()", "SE_connection_set_concurrency()");
      return(MS_FAILURE);
    }
    

    status = SE_stream_create(poolinfo->connection, &(poolinfo->stream));
    if(status != SE_SUCCESS) {
      sde_error(status, "msSDELayerOpen()", "SE_stream_create()");
    return(MS_FAILURE);
    }

    /* Register the connection with the connection pooling API.  Give  */
    /* msSDECloseConnection as the function to call when we run out of layer  */
    /* instances using it */
    msConnPoolRegister(layer, poolinfo, msSDECloseConnection);
    msFreeCharArray(params, numparams); /* done with parameter list */
  }

  /* Split the DATA member into its parameters using the comma */
  /* Periods (.) are used to denote table names and schemas in SDE,  */
  /* as are underscores (_). */
  data_params = split(layer->data, ',', &numparams);
  if(!data_params) {
    msSetError(MS_MEMERR, 
        "Error spliting SDE layer information.", "msSDELayerOpen()");
    return(MS_FAILURE);
  }

  if(numparams < 2) {
    msSetError(MS_SDEERR, 
    "Not enough SDE layer parameters specified.", "msSDELayerOpen()");
    return(MS_FAILURE);
  }

  sde->table = strdup(data_params[0]); 
  sde->column = strdup(data_params[1]);


  if (numparams < 3){ 
    /* User didn't specify a version, we won't use one */
    if (layer->debug) {
      msDebug("msSDELayerOpen(): Layer %s did not have a " 
              "specified version.\n", layer->name);
    } 
    sde->state_id = SE_DEFAULT_STATE_ID;
  } 
  else {
    if (layer->debug) {
      msDebug("msSDELayerOpen(): Layer %s specified version %s.\n", 
              layer->name, 
              data_params[2]);
    }
    status = SE_versioninfo_create (&(version));
    if(status != SE_SUCCESS) {
      sde_error(status, "msSDELayerOpen()", "SE_versioninfo_create()");
      return(MS_FAILURE);
    }
    status = SE_version_get_info(poolinfo->connection, data_params[2], version);
    
    if(status != SE_SUCCESS) {
       
      if (status == SE_INVALID_RELEASE) {
        /* The user has incongruent versions of SDE, ie 8.2 client and  */
        /* 8.3 server set the state_id to SE_DEFAULT_STATE_ID, which means    */
        /* no version queries are done */
        sde->state_id = SE_DEFAULT_STATE_ID;
      }
      else {
        sde_error(status, "msSDELayerOpen()", "SE_version_get_info()");
        return(MS_FAILURE);
      }
    }
  
  }

  /* Get the STATEID from the given version and set the stream to  */
  /* that if we didn't already set it to SE_DEFAULT_STATE_ID.   */
  if (!(sde->state_id == SE_DEFAULT_STATE_ID)){
    status = SE_versioninfo_get_state_id(version, &sde->state_id);
    if(status != SE_SUCCESS) {
      sde_error(status, "msSDELayerOpen()", "SE_versioninfo_get_state_id()");
      return(MS_FAILURE);
    }
    SE_versioninfo_free(version);
    status = SE_stateinfo_create (&state);
    if(status != SE_SUCCESS) {
      sde_error(status, "msSDELayerOpen()", "SE_stateinfo_create()");
      return(MS_FAILURE);
    }    
    status = SE_state_get_info(poolinfo->connection, sde->state_id, state);
    if(status != SE_SUCCESS) {
      sde_error(status, "msSDELayerOpen()", "SE_state_get_info()");
      return(MS_FAILURE);
    }  
    if (SE_stateinfo_is_open (state)) {
      /* If the state is open for edits, we shouldn't be querying from it */
      sde_error(status, 
                "msSDELayerOpen()", 
                "SE_stateinfo_is_open() -- State for version is open");
      return(MS_FAILURE);
    }
    SE_stateinfo_free (state); 

        msFreeCharArray(data_params, numparams);  
  } /* if (!(sde->state_id == SE_DEFAULT_STATE_ID)) */
  
  
  status = SE_layerinfo_create(NULL, &(sde->layerinfo));
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerOpen()", "SE_layerinfo_create()");
    return(MS_FAILURE);
  }


  status = msSDEGetLayerInfo( layer,
                              poolinfo->connection,
                              sde->table,
                              sde->column,
                              layer->connection,
                              sde->layerinfo);

  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerOpen()", "SE_layer_get_info()");
    return(MS_FAILURE);
  }

  SE_coordref_create(&(sde->coordref));
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerOpen()", "SE_coordref_create()");
    return(MS_FAILURE);
  }

  status = SE_layerinfo_get_coordref(sde->layerinfo, sde->coordref);
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerOpen()", "SE_layerinfo_get_coordref()");
    return(MS_FAILURE);
  }


  /* reset the stream */
  status = SE_stream_close(poolinfo->stream, 1);
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerOpen()", "SE_stream_close()");
    return(MS_FAILURE);
  }  


  /* point to the SDE layer information  */
  /* (note this might actually be in another layer) */
  layer->layerinfo = sde; 

  sde->connection = poolinfo->connection;
  sde->stream = poolinfo->stream;
  sde->connPoolInfo = poolinfo;


  return(MS_SUCCESS);
#else
  msSetError(MS_MISCERR, "SDE support is not available.", "msSDELayerOpen()");
  return(MS_FAILURE);
#endif
}


/* -------------------------------------------------------------------- */
/* msSDELayerIsOpen                                                     */
/* -------------------------------------------------------------------- */
/*     Returns MS_TRUE if layer is already opened, MS_FALSE otherwise   */
/* -------------------------------------------------------------------- */
int msSDELayerIsOpen(layerObj *layer) {
#ifdef USE_SDE

  if(layer->layerinfo) 
      return(MS_TRUE); 

  return MS_FALSE;

#else
  msSetError(MS_MISCERR, "SDE support is not available.",
             "msSDELayerIsOpen()");
  return(MS_FALSE);
#endif
}

/* -------------------------------------------------------------------- */
/* msSDELayerClose                                                      */
/* -------------------------------------------------------------------- */
/*     Closes the MapServer layer.  This doesn't necessarily close the  */
/*     connection to the layer.                                         */
/* -------------------------------------------------------------------- */
int  msSDELayerClose(layerObj *layer) {
#ifdef USE_SDE


  msSDELayerInfo *sde=NULL;

  sde = layer->layerinfo;
  if (sde == NULL) return MS_SUCCESS;  /* Silently return if layer not opened. */

  if(layer->debug) 
    msDebug("msSDELayerClose(): Closing layer %s.\n", layer->name);
	
  if (sde->layerinfo) SE_layerinfo_free(sde->layerinfo);
  if (sde->coordref) SE_coordref_free(sde->coordref);
  if (sde->table) free(sde->table);
  if (sde->column) free(sde->column);
  if (sde->row_id_column) free(sde->row_id_column);

  msConnPoolRelease( layer, sde->connPoolInfo );  
  sde->connection = NULL;
  sde->connPoolInfo = NULL;
  if (layer->layerinfo) free(layer->layerinfo);
  layer->layerinfo = NULL;
  return MS_SUCCESS;

#else
  msSetError( MS_MISCERR, 
              "SDE support is not available.", 
              "msSDELayerClose()");
  return(MS_FALSE);
#endif
}


/* -------------------------------------------------------------------- */
/* msSDELayerCloseConnection                                            */
/* -------------------------------------------------------------------- */
/* Virtual table function                                               */
/* -------------------------------------------------------------------- */
/*int msSDELayerCloseConnection(layerObj *layer) 
{
	

#ifdef USE_SDE


  msSDELayerInfo *sde=NULL;

  sde = layer->layerinfo;
  if (sde == NULL) return;   Silently return if layer not opened./

  if(layer->debug)
    msDebug("msSDELayerCloseConnection(): Closing connection for layer %s.\n", layer->name);

  msConnPoolRelease( layer, sde->connPoolInfo );
  sde->connection = NULL;
  sde->connPoolInfo = NULL;

#else
  msSetError( MS_MISCERR,
              "SDE support is not available.",
              "msSDELayerClose()");
  return;
#endif

    return MS_SUCCESS;
}
*/

/* -------------------------------------------------------------------- */
/* msSDELayerWhichShapes                                                */
/* -------------------------------------------------------------------- */
/*     starts a stream query using spatial filter.  Also limits the     */
/*     query by the layer's FILTER item as well.                        */
/* -------------------------------------------------------------------- */
int msSDELayerWhichShapes(layerObj *layer, rectObj rect) {
#ifdef USE_SDE
  long status;
  SE_ENVELOPE envelope;
  SE_SHAPE shape=0;
  SE_FILTER constraint;
  SE_QUERYINFO query_info;
  char* proc_value=NULL;
  int query_order=SE_SPATIAL_FIRST;

  msSDELayerInfo *sde=NULL;

  sde = layer->layerinfo;
  if(!sde) {
    msSetError( MS_SDEERR, 
                "SDE layer has not been opened.", 
                "msSDELayerWhichShapes()");
    return(MS_FAILURE);
  }

  status = SE_shape_create(sde->coordref, &shape);
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerWhichShapes()", "SE_shape_create()");
    return(MS_FAILURE);
  }

  status = SE_layerinfo_get_envelope(sde->layerinfo, &envelope);
  if(status != SE_SUCCESS) {
    sde_error(status, 
              "msSDELayerWhichShapes()", 
              "SE_layerinfo_get_envelope()");
    return(MS_FAILURE);
  }
  
  /* there is NO overlap, return MS_DONE */
  /* (FIX: use this in ALL which shapes functions) */
  if(envelope.minx > rect.maxx) return(MS_DONE); 
  if(envelope.maxx < rect.minx) return(MS_DONE);
  if(envelope.miny > rect.maxy) return(MS_DONE);
  if(envelope.maxy < rect.miny) return(MS_DONE);

  /* set spatial constraint search shape */
  /* crop against SDE layer extent *argh* */
  envelope.minx = MS_MAX(rect.minx, envelope.minx); 
  envelope.miny = MS_MAX(rect.miny, envelope.miny);
  envelope.maxx = MS_MIN(rect.maxx, envelope.maxx);
  envelope.maxy = MS_MIN(rect.maxy, envelope.maxy);
  
  if( envelope.minx == envelope.maxx && envelope.miny == envelope.maxy){
        /* fudge a rectangle so we have a valid one for generate_rectangle */
        /* FIXME: use the real shape for the query and set the filter_type 
           to be an appropriate type */
        envelope.minx = envelope.minx - 0.001;
        envelope.maxx = envelope.maxx + 0.001;
        envelope.miny = envelope.miny - 0.001;
        envelope.maxy = envelope.maxy + 0.001;
    }

  status = SE_shape_generate_rectangle(&envelope, shape);
  if(status != SE_SUCCESS) {
    sde_error(status, 
              "msSDELayerWhichShapes()", 
              "SE_shape_generate_rectangle()");
    return(MS_FAILURE);
  }
  constraint.filter.shape = shape;

  /* set spatial constraint column and table */
  strcpy(constraint.table, sde->table);
  strcpy(constraint.column, sde->column);

  /* set a couple of other spatial constraint properties */
  constraint.method = SM_ENVP;
  constraint.filter_type = SE_SHAPE_FILTER;
  constraint.truth = TRUE;

  /* See http://forums.esri.com/Thread.asp?c=2&f=59&t=108929&mc=4#msgid310273 */
  /* SE_queryinfo is a new SDE struct in ArcSDE 8.x that is a bit easier  */
  /* (and faster) to use and will allow us to support joins in the future.  HCB */
  status = SE_queryinfo_create (&query_info);
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerWhichShapes()", "SE_queryinfo_create()");
    return(MS_FAILURE);
  }

  /* set the tables -- just one at this point */
  status = SE_queryinfo_set_tables (query_info, 
                                    1, 
                                    (const CHAR **) &(sde->table),
                                    NULL);
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerWhichShapes()", "SE_queryinfo_create()");
    return(MS_FAILURE);
  }

  /* set the "where" clause */
  if(!(layer->filter.string))
    /* set to empty string */
    status = SE_queryinfo_set_where_clause (query_info, 
                                            (const CHAR * ) "");
  else
    /* set to the layer's filter.string */
    status = SE_queryinfo_set_where_clause (query_info, 
                                 (const CHAR * ) strdup(layer->filter.string));
  if(status != SE_SUCCESS) {
    sde_error(status, 
              "msSDELayerWhichShapes()", 
              "SE_queryinfo_set_where_clause()");
    return(MS_FAILURE);
  }

  status = SE_queryinfo_set_columns(query_info, 
                                    layer->numitems, 
                                    (const char **)layer->items);
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerWhichShapes()", "SE_queryinfo_set_columns()");
    return(MS_FAILURE);
  }
  
  /* Join the spatial and feature tables.  If we specify the type of join */
  /* we'll query faster than querying all tables individually (old method) */
  status = SE_queryinfo_set_query_type (query_info,SE_QUERYTYPE_JSF);
  if(status != SE_SUCCESS) {
    sde_error(status, 
              "msSDELayerWhichShapes()", 
              "SE_queryinfo_set_query_type()");
    return(MS_FAILURE);
  }
  


  /* reset the stream */
  status = SE_stream_close(sde->stream, 1);
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerGetShape()", "SE_stream_close()");
    return(MS_FAILURE);
  }
  /* Set the stream state back to the state_id of our user-specified version */
  /* This must be done every time after the stream is reset before the  */
  /* query happens. */

  if (!(sde->state_id == SE_DEFAULT_STATE_ID)){

    status =  SE_stream_set_state(sde->stream, 
                                  sde->state_id, 
                                  sde->state_id, 
                                  SE_STATE_DIFF_NOCHECK); 
    if(status != SE_SUCCESS) {
      sde_error(status, "msSDELayerOpen()", "SE_stream_set_state()");
      return(MS_FAILURE);
    }  
  } 

  status = SE_stream_query_with_info(sde->stream, query_info);
  if(status != SE_SUCCESS) {
    sde_error(status, 
              "msSDELayerWhichShapes()", 
              "SE_stream_query_with_info()");
    return(MS_FAILURE);
  }
  
  proc_value = msLayerGetProcessingKey(layer,"QUERYORDER");
  if(proc_value && strcasecmp(proc_value, "ATTRIBUTE") == 0)
    query_order = SE_ATTRIBUTE_FIRST;

  status = SE_stream_set_spatial_constraints( sde->stream, 
                                              query_order, 
                                              FALSE, 
                                              1, 
                                              &constraint);

  if(status != SE_SUCCESS) {
    sde_error(status, 
              "msSDELayerWhichShapes()", 
              "SE_stream_set_spatial_constraints()");
    return(MS_FAILURE);
  }
  
  /* *should* be ready to step through shapes now */
  status = SE_stream_execute(sde->stream); 
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerWhichShapes()", "SE_stream_execute()");
    return(MS_FAILURE);
  }

  /* clean-up */
  SE_shape_free(shape);
  SE_queryinfo_free (query_info);
  
  return(MS_SUCCESS);
#else
  msSetError(MS_MISCERR, 
             "SDE support is not available.", 
             "msSDELayerWhichShapes()");
  return(MS_FAILURE);
#endif
}

/* -------------------------------------------------------------------- */
/* msSDELayerNextShape                                                  */
/* -------------------------------------------------------------------- */
/*     Recursively gets the shapes for the SDE layer                    */
/* -------------------------------------------------------------------- */
int msSDELayerNextShape(layerObj *layer, shapeObj *shape) {
#ifdef USE_SDE
  long status;

  msSDELayerInfo *sde=NULL;
  
  sde = layer->layerinfo;
  if(!sde) {
    msSetError( MS_SDEERR, 
                "SDE layer has not been opened.", 
                "msSDELayerNextShape()");
    return(MS_FAILURE);
  }

  /* fetch the next record from the stream */
  status = SE_stream_fetch(sde->stream);

  if(status == SE_FINISHED)
    return(MS_DONE);
  else if(status != MS_SUCCESS) {
    sde_error(status, "msSDELayerNextShape()", "SE_stream_fetch()");
    return(MS_FAILURE);
  }

  /* get the shape and values (first column is the shape id,  */
  /* second is the shape itself) */
  status = sdeGetRecord(layer, shape);
  if(status != MS_SUCCESS)
    return(MS_FAILURE); /* something went wrong fetching the record/shape */

  if(shape->numlines == 0) /* null shape, skip it */
    return(msSDELayerNextShape(layer, shape));

  return(MS_SUCCESS);
#else
  msSetError( MS_MISCERR, 
              "SDE support is not available.", 
              "msSDELayerNextShape()");
  return(MS_FAILURE);
#endif
}

/* -------------------------------------------------------------------- */
/* msSDELayerGetItems                                                   */
/* -------------------------------------------------------------------- */
/*     Queries the SDE table's column names into layer->iteminfo        */
/* -------------------------------------------------------------------- */
int msSDELayerGetItems(layerObj *layer) {
#ifdef USE_SDE
  int i,j;
  short n;
  long status;

  SE_COLUMN_DEF *itemdefs;

  msSDELayerInfo *sde=NULL;

  sde = layer->layerinfo;
  if(!sde) {
    msSetError( MS_SDEERR, 
                "SDE layer has not been opened.", 
                "msSDELayerGetItems()");
    return(MS_FAILURE);
  }
  
  if (!sde->row_id_column) {
    sde->row_id_column = (char*) malloc(SE_MAX_COLUMN_LEN);
  }
  sde->row_id_column = msSDELayerGetRowIDColumn(layer);

  status = SE_table_describe(sde->connection, sde->table, &n, &itemdefs);
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerGetItems()", "SE_table_describe()");
    return(MS_FAILURE);
  }

  layer->numitems = n;

  layer->items = (char **)malloc(layer->numitems*sizeof(char *));
  if(!layer->items) {
    msSetError( MS_MEMERR, "Error allocating layer items array.", "msSDELayerGetItems()");
    return(MS_FAILURE);
  }

  for(i=0; i<n; i++) layer->items[i] = strdup(itemdefs[i].column_name);

  if (!layer->iteminfo){
    layer->iteminfo = (SE_COLUMN_DEF *) calloc( layer->numitems, sizeof(SE_COLUMN_DEF));
    if(!layer->iteminfo) {
      msSetError( MS_MEMERR, "Error allocating SDE item information.", "msSDELayerGetItems()");
      return(MS_FAILURE);
    }
  }

  for(i=0; i<layer->numitems; i++) { /* requested columns */

    for(j=0; j<n; j++) { /* all columns */
      if(strcasecmp(layer->items[i], itemdefs[j].column_name) == 0) { 
        /* found it */
        ((SE_COLUMN_DEF *)(layer->iteminfo))[i] = itemdefs[j];
        break;
      }
    }
  }
  
  SE_table_free_descriptions(itemdefs);

  return(MS_SUCCESS);
#else
  msSetError( MS_MISCERR, 
              "SDE support is not available.", 
              "msSDELayerGetItems()");
  return(MS_FAILURE);
#endif
}

/* -------------------------------------------------------------------- */
/* msSDELayerGetExtent                                                  */
/* -------------------------------------------------------------------- */
/*     Returns the extent of the SDE layer                              */
/* -------------------------------------------------------------------- */
int msSDELayerGetExtent(layerObj *layer, rectObj *extent) {
#ifdef USE_SDE
  long status;

  SE_ENVELOPE envelope;

  msSDELayerInfo *sde=NULL;

  sde = layer->layerinfo;
  if(!sde) {
    msSetError(MS_SDEERR,
               "SDE layer has not been opened.", 
               "msSDELayerGetExtent()");
    return(MS_FAILURE);
  }

  status = SE_layerinfo_get_envelope(sde->layerinfo, &envelope);
  if(status != SE_SUCCESS) {
    sde_error(status, 
              "msSDELayerGetExtent()", 
              "SE_layerinfo_get_envelope()");
    return(MS_FAILURE);
  }
  
  extent->minx = envelope.minx;
  extent->miny = envelope.miny;
  extent->maxx = envelope.maxx;
  extent->maxy = envelope.maxy;

  return(MS_SUCCESS);
#else
  msSetError( MS_MISCERR, 
              "SDE support is not available.", 
              "msSDELayerGetExtent()");
  return(MS_FAILURE);
#endif
}

/* -------------------------------------------------------------------- */
/* msSDELayerGetShape                                                   */
/* -------------------------------------------------------------------- */
/*     Queries SDE for a shape (and its attributes, if requested)       */
/*     given the ID (which is the MS_SDE_ROW_ID_COLUMN column           */
/* -------------------------------------------------------------------- */
int msSDELayerGetShape(layerObj *layer, shapeObj *shape, long record) {

#ifdef USE_SDE
  long status;
  msSDELayerInfo *sde=NULL;

  sde = layer->layerinfo;
  if(!sde) {
    msSetError( MS_SDEERR, 
                "SDE layer has not been opened.", 
                "msSDELayerGetExtent()");
    return(MS_FAILURE);
  }

  /* must be at least one thing to retrieve (i.e. spatial column) */
  if(layer->numitems < 1) { 
    msSetError( MS_MISCERR, 
                "No items requested, SDE requires at least one item.", 
                "msSDELayerGetShape()");
    return(MS_FAILURE);
  }



  /* reset the stream */
  status = SE_stream_close(sde->stream, 1);
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerGetShape()", "SE_stream_close()");
    return(MS_FAILURE);
  }

  status = SE_stream_fetch_row( sde->stream, 
                                sde->table, 
                                record, 
                                (short)(layer->numitems), 
                                (const char **)layer->items);
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerGetShape()", "SE_stream_fetch_row()");
    return(MS_FAILURE);
  }
 
  status = sdeGetRecord(layer, shape);
  if(status != MS_SUCCESS)
    return(MS_FAILURE); /* something went wrong fetching the record/shape */

  return(MS_SUCCESS);
#else
  msSetError( MS_MISCERR,  
              "SDE support is not available.", 
              "msSDELayerGetShape()");
  return(MS_FAILURE);
#endif
}

/* -------------------------------------------------------------------- */
/* msSDELayerGetShapeVT                                                 */
/* -------------------------------------------------------------------- */
/* Overloaded version for virtual table                                 */
/* -------------------------------------------------------------------- */
int msSDELayerGetShapeVT(layerObj *layer, shapeObj *shape, int tile, long record) {
	return msSDELayerGetShape(layer, shape, record);
}

/* -------------------------------------------------------------------- */
/* msSDELayerInitItemInfo                                               */
/* -------------------------------------------------------------------- */
/*     Inits the stuff we'll be querying from SDE                       */
/* -------------------------------------------------------------------- */
int msSDELayerInitItemInfo(layerObj *layer)
{
#ifdef USE_SDE
  long status;
  short n;
  int i, j;

  SE_COLUMN_DEF *itemdefs;

  msSDELayerInfo *sde=NULL;

  sde = layer->layerinfo;
  sde->row_id_column = msSDELayerGetRowIDColumn(layer);

  
  if(!sde) {
    msSetError( MS_SDEERR, 
                "SDE layer has not been opened.", 
                "msSDELayerInitItemInfo()");
    return(MS_FAILURE);
  }

  
  status = SE_table_describe(sde->connection, sde->table, &n, &itemdefs);
  if(status != SE_SUCCESS) {
    sde_error(status, "msSDELayerGetItemInfo()", "SE_table_describe()");
    return(MS_FAILURE);
  }

if (!layer->iteminfo){
  layer->iteminfo = (SE_COLUMN_DEF *) calloc( layer->numitems, 
                                              sizeof(SE_COLUMN_DEF));
  if(!layer->iteminfo) {
    msSetError( MS_MEMERR,
                "Error allocating SDE item information.", 
                "msSDELayerInitItemInfo()");
    return(MS_FAILURE);
  }
}

  for(i=0; i<layer->numitems; i++) { /* requested columns */
    if(strcmp(layer->items[i],sde->row_id_column) == 0)      
      continue;

    for(j=0; j<n; j++) { /* all columns */
      if(strcasecmp(layer->items[i], itemdefs[j].column_name) == 0) { 
        /* found it */
        ((SE_COLUMN_DEF *)(layer->iteminfo))[i] = itemdefs[j];
        break;
      }
    }

    if(j == n) {
      msSetError( MS_MISCERR, 
                  "Item not found in SDE table.", 
                  "msSDELayerInitItemInfo()");
      return(MS_FAILURE);
    }
  }

  SE_table_free_descriptions(itemdefs);

  return(MS_SUCCESS);
#else
  msSetError( MS_MISCERR, 
              "SDE support is not available.", 
              "msSDELayerInitItemInfo()");
  return(MS_FAILURE);
#endif
}

/* -------------------------------------------------------------------- */
/* msSDELayerFreeItemInfo                                               */
/* -------------------------------------------------------------------- */
void msSDELayerFreeItemInfo(layerObj *layer)
{
#ifdef USE_SDE
  if(layer->iteminfo) {
    SE_table_free_descriptions((SE_COLUMN_DEF *)layer->iteminfo);  
    layer->iteminfo = NULL;
  }
#else
  msSetError( MS_MISCERR, 
              "SDE support is not available.", 
              "msSDELayerFreeItemInfo()");
#endif
}

/* -------------------------------------------------------------------- */
/* msSDELayerGetSpatialColumn                                           */
/* -------------------------------------------------------------------- */
/*     A helper function to return the spatial column for               */ 
/*     an opened SDE layer                                              */
/* -------------------------------------------------------------------- */
char *msSDELayerGetSpatialColumn(layerObj *layer)
{
#ifdef USE_SDE
  msSDELayerInfo *sde=NULL;

  sde = layer->layerinfo;
  if(!sde) {
    msSetError( MS_SDEERR, 
                "SDE layer has not been opened.", 
                "msSDELayerGetSpatialColumn()");
    return(NULL);
  }

  return(strdup(sde->column));
#else
  msSetError( MS_MISCERR, 
              "SDE support is not available.", 
              "msSDELayerGetSpatialColumn()");
  return(NULL);
#endif
}



/* -------------------------------------------------------------------- */
/* msSDELayerCreateItems                                                */
/* -------------------------------------------------------------------- */
/* Special item allocator for SDE                                       */
/* -------------------------------------------------------------------- */
int
msSDELayerCreateItems(layerObj *layer,
                      int nt) 
{
#ifdef USE_SDE
    /* should be more than enough space, 
     * SDE always needs a couple of additional items  
     */

    layer->items = (char **)calloc(nt+2, sizeof(char *)); 
    if( ! layer->items) {
        msSetError(MS_MEMERR, NULL, "msSDELayerCreateItems()");
        return(MS_FAILURE);
      }
    layer->items[0] = msSDELayerGetRowIDColumn(layer); /* row id */
    layer->items[1] = msSDELayerGetSpatialColumn(layer);
    layer->numitems = 2;
    return MS_SUCCESS;

#else
  msSetError( MS_MISCERR, 
              "SDE support is not available.", 
              "msSDELayerGetRowIDColumn()");
  return(MS_FAILURE);
#endif
}

int
msSDELayerInitializeVirtualTable(layerObj *layer)
{
    assert(layer != NULL);
    assert(layer->vtable != NULL);

    layer->vtable->LayerInitItemInfo = msSDELayerInitItemInfo;
    layer->vtable->LayerFreeItemInfo = msSDELayerFreeItemInfo;
    layer->vtable->LayerOpen = msSDELayerOpen;
    layer->vtable->LayerIsOpen = msSDELayerIsOpen;
    layer->vtable->LayerWhichShapes = msSDELayerWhichShapes;
    layer->vtable->LayerNextShape = msSDELayerNextShape;
    layer->vtable->LayerGetShape = msSDELayerGetShapeVT;
    layer->vtable->LayerClose = msSDELayerClose;
    layer->vtable->LayerGetItems = msSDELayerGetItems;
    layer->vtable->LayerGetExtent = msSDELayerGetExtent;

    /* layer->vtable->LayerGetAutoStyle, use default */
    /* layer->vtable->LayerApplyFilterToLayer, use default */

    /* SDE uses pooled connections, close from msCloseConnections */
    /*layer->vtable->LayerCloseConnection = msSDELayerCloseConnection;*/

    layer->vtable->LayerSetTimeFilter = msLayerMakePlainTimeFilter;
    layer->vtable->LayerCreateItems = msSDELayerCreateItems;
    /* layer->vtable->LayerGetNumFeatures, use default */

    return MS_SUCCESS;
}

