/******************************************************************************
 * $Id: ogrinfo.cpp 26381 2013-08-28 12:41:20Z rouault $
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Simple client for viewing OGR driver data.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999, Frank Warmerdam
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/

#include "ogr_api.h"
#include "ogrsf_frmts.h"
#include "ogr_p.h"
#include "cpl_conv.h"
#include "cpl_string.h"
#include "cpl_multiproc.h"
#include "commonutils.h"

CPL_CVSID("$Id: ogrinfo.cpp 26381 2013-08-28 12:41:20Z rouault $");

int     bReadOnly = FALSE;
int     bVerbose = TRUE;
int     bSummaryOnly = FALSE;
int     nFetchFID = OGRNullFID;
char**  papszOptions = NULL;

enum InfoOutputFormat {
    ofGDAL, ofXML, ofJSON
};

static void Usage(const char* pszErrorMsg = NULL);

static void ReportOnLayer( OGRLayer *, const char *, const char* pszGeomField, 
                           OGRGeometry *, InfoOutputFormat);

/************************************************************************/
/*                                main()                                */
/************************************************************************/

#define CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(nExtraArg) \
    do { if (iArg + nExtraArg >= nArgc) \
        Usage(CPLSPrintf("%s option requires %d argument(s)", papszArgv[iArg], nExtraArg)); } while(0)

int main( int nArgc, char ** papszArgv )

{
    const char *pszWHERE = NULL;
    const char  *pszDataSource = NULL;
    char        **papszLayers = NULL;
    OGRGeometry *poSpatialFilter = NULL;
    int         nRepeatCount = 1, bAllLayers = FALSE;
    const char  *pszSQLStatement = NULL;
    const char  *pszDialect = NULL;
    int          nRet = 0;
    const char* pszGeomField = NULL;
    
    /* Check strict compilation and runtime library version as we use C++ API */
    if (! GDAL_CHECK_VERSION(papszArgv[0]))
        exit(1);

    EarlySetConfigOptions(nArgc, papszArgv);

/* -------------------------------------------------------------------- */
/*      Register format(s).                                             */
/* -------------------------------------------------------------------- */
    OGRRegisterAll();
    
/* -------------------------------------------------------------------- */
/*      Processing command line arguments.                              */
/* -------------------------------------------------------------------- */
    nArgc = OGRGeneralCmdLineProcessor( nArgc, &papszArgv, 0 );
    InfoOutputFormat outputFormat = ofGDAL;
    
    if( nArgc < 1 )
        exit( -nArgc );

    for( int iArg = 1; iArg < nArgc; iArg++ )
    {
        if( EQUAL(papszArgv[iArg], "--utility_version") )
        {
            printf("%s was compiled against GDAL %s and is running against GDAL %s\n",
                   papszArgv[0], GDAL_RELEASE_NAME, GDALVersionInfo("RELEASE_NAME"));
            return 0;
        }
        else if( EQUAL(papszArgv[iArg],"--help") )
            Usage();
        else if( EQUAL(papszArgv[iArg],"-ro") )
            bReadOnly = TRUE;
        else if( EQUAL(papszArgv[iArg],"-q") || EQUAL(papszArgv[iArg],"-quiet"))
            bVerbose = FALSE;
        else if( EQUAL(papszArgv[iArg],"-fid") )
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            nFetchFID = atoi(papszArgv[++iArg]);
        }
        else if( EQUAL(papszArgv[iArg],"-spat") )
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(4);
            OGRLinearRing  oRing;

            oRing.addPoint( atof(papszArgv[iArg+1]), atof(papszArgv[iArg+2]) );
            oRing.addPoint( atof(papszArgv[iArg+1]), atof(papszArgv[iArg+4]) );
            oRing.addPoint( atof(papszArgv[iArg+3]), atof(papszArgv[iArg+4]) );
            oRing.addPoint( atof(papszArgv[iArg+3]), atof(papszArgv[iArg+2]) );
            oRing.addPoint( atof(papszArgv[iArg+1]), atof(papszArgv[iArg+2]) );

            poSpatialFilter = new OGRPolygon();
            ((OGRPolygon *) poSpatialFilter)->addRing( &oRing );
            iArg += 4;
        }
        else if( EQUAL(papszArgv[iArg],"-geomfield") )
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            pszGeomField = papszArgv[++iArg];
        }
        else if( EQUAL(papszArgv[iArg],"-where") )
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            pszWHERE = papszArgv[++iArg];
        }
        else if( EQUAL(papszArgv[iArg],"-sql") )
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            pszSQLStatement = papszArgv[++iArg];
        }
        else if( EQUAL(papszArgv[iArg],"-dialect") )
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            pszDialect = papszArgv[++iArg];
        }
        else if( EQUAL(papszArgv[iArg],"-rc") )
        {
            CHECK_HAS_ENOUGH_ADDITIONAL_ARGS(1);
            nRepeatCount = atoi(papszArgv[++iArg]);
        }
        else if( EQUAL(papszArgv[iArg],"-al") )
        {
            bAllLayers = TRUE;
        }
        else if( EQUAL(papszArgv[iArg],"-so") 
                 || EQUAL(papszArgv[iArg],"-summary")  )
        {
            bSummaryOnly = TRUE;
        }
        else if( EQUALN(papszArgv[iArg],"-fields=", strlen("-fields=")) )
        {
            char* pszTemp = (char*)CPLMalloc(32 + strlen(papszArgv[iArg]));
            sprintf(pszTemp, "DISPLAY_FIELDS=%s", papszArgv[iArg] + strlen("-fields="));
            papszOptions = CSLAddString(papszOptions, pszTemp);
            CPLFree(pszTemp);
        }
        else if( EQUALN(papszArgv[iArg],"-geom=", strlen("-geom=")) )
        {
            char* pszTemp = (char*)CPLMalloc(32 + strlen(papszArgv[iArg]));
            sprintf(pszTemp, "DISPLAY_GEOMETRY=%s", papszArgv[iArg] + strlen("-geom="));
            papszOptions = CSLAddString(papszOptions, pszTemp);
            CPLFree(pszTemp);
        }
        else if( EQUAL(papszArgv[iArg],"-xml") )
        {
            outputFormat = ofXML;
        }
        else if( papszArgv[iArg][0] == '-' )
        {
            Usage(CPLSPrintf("Unknown option name '%s'", papszArgv[iArg]));
        }
        else if( pszDataSource == NULL )
            pszDataSource = papszArgv[iArg];
        else
        {
            papszLayers = CSLAddString( papszLayers, papszArgv[iArg] );
            bAllLayers = FALSE;
        }
    }

    if( pszDataSource == NULL )
        Usage("No datasource specified.");

/* -------------------------------------------------------------------- */
/*      Open data source.                                               */
/* -------------------------------------------------------------------- */
    OGRDataSource       *poDS = NULL;
    OGRSFDriver         *poDriver = NULL;

    poDS = OGRSFDriverRegistrar::Open( pszDataSource, !bReadOnly, &poDriver );
    if( poDS == NULL && !bReadOnly )
    {
        poDS = OGRSFDriverRegistrar::Open( pszDataSource, FALSE, &poDriver );
        if( poDS != NULL && bVerbose )
        {
            // This should print to STDERR as it will cause parsing errors
            // with XML/JSON output formats.
            fprintf(stderr, "Had to open data source read-only.\n" );
            bReadOnly = TRUE;
        }
    }

/* -------------------------------------------------------------------- */
/*      Report failure                                                  */
/* -------------------------------------------------------------------- */
    if( poDS == NULL )
    {
        OGRSFDriverRegistrar    *poR = OGRSFDriverRegistrar::GetRegistrar();
        
        fprintf(stderr, "FAILURE:\n"
                "Unable to open datasource `%s' with the following drivers.\n",
                pszDataSource );

        for( int iDriver = 0; iDriver < poR->GetDriverCount(); iDriver++ )
        {
            printf( "  -> %s\n", poR->GetDriver(iDriver)->GetName() );
        }

        nRet = 1;
        goto end;
    }

    CPLAssert( poDriver != NULL);

/* -------------------------------------------------------------------- */
/*      Some information messages.                                      */
/* -------------------------------------------------------------------- */
    if( bVerbose )
        fprintf(stderr, "INFO: Open of `%s'\n"
                "      using driver `%s' successful.\n",
                pszDataSource, poDriver->GetName() );

    if( bVerbose && !EQUAL(pszDataSource,poDS->GetName()) )
    {
        fprintf(stderr, "INFO: Internal data source name `%s'\n"
                "      different from user name `%s'.\n",
                poDS->GetName(), pszDataSource );
    }

/* -------------------------------------------------------------------- */
/*      Special case for -sql clause.  No source layers required.       */
/* -------------------------------------------------------------------- */
    if( pszSQLStatement != NULL )
    {
        OGRLayer *poResultSet = NULL;

        nRepeatCount = 0;  // skip layer reporting.

        if( CSLCount(papszLayers) > 0 )
            fprintf(stderr, "layer names ignored in combination with -sql.\n" );
        
        poResultSet = poDS->ExecuteSQL( pszSQLStatement,
                                        (pszGeomField == NULL) ? poSpatialFilter: NULL, 
                                        pszDialect );

        if( poResultSet != NULL )
        {
            if( pszWHERE != NULL )
            {
                if (poResultSet->SetAttributeFilter( pszWHERE ) != OGRERR_NONE )
                {
                    fprintf(stderr, "FAILURE: SetAttributeFilter(%s) failed.\n", pszWHERE );
                    exit(1);
                }
            }

            if( pszGeomField != NULL )
                ReportOnLayer( poResultSet, NULL, pszGeomField, poSpatialFilter, outputFormat );
            else
                ReportOnLayer( poResultSet, NULL, NULL, NULL, outputFormat );
            poDS->ReleaseResultSet( poResultSet );
        }
    }

    CPLDebug( "OGR", "GetLayerCount() = %d\n", poDS->GetLayerCount() );

    for( int iRepeat = 0; iRepeat < nRepeatCount; iRepeat++ )
    {
        if ( CSLCount(papszLayers) == 0 )
        {
/* -------------------------------------------------------------------- */ 
/*      Process each data source layer.                                 */ 
/* -------------------------------------------------------------------- */ 
            for( int iLayer = 0; iLayer < poDS->GetLayerCount(); iLayer++ )
            {
                OGRLayer        *poLayer = poDS->GetLayer(iLayer);

                if( poLayer == NULL )
                {
                    fprintf(stderr, "FAILURE: Couldn't fetch advertised layer %d!\n",
                            iLayer );
                    exit( 1 );
                }

                if (!bAllLayers)
                {
                    printf( "%d: %s",
                            iLayer+1,
                            poLayer->GetName() );

                    int nGeomFieldCount =
                        poLayer->GetLayerDefn()->GetGeomFieldCount();
                    if( nGeomFieldCount > 1 )
                    {
                        printf( " (");
                        for(int iGeom = 0;iGeom < nGeomFieldCount; iGeom ++ )
                        {
                            if( iGeom > 0 )
                                printf(", ");
                            OGRGeomFieldDefn* poGFldDefn =
                                poLayer->GetLayerDefn()->GetGeomFieldDefn(iGeom);
                            printf( "%s", 
                                OGRGeometryTypeToName( 
                                    poGFldDefn->GetType() ) );
                        }
                        printf( ")");
                    }
                    else if( poLayer->GetGeomType() != wkbUnknown )
                        printf( " (%s)", 
                                OGRGeometryTypeToName( 
                                    poLayer->GetGeomType() ) );

                    printf( "\n" );
                }
                else
                {
                    if( iRepeat != 0 )
                        poLayer->ResetReading();

                    ReportOnLayer( poLayer, pszWHERE, pszGeomField, poSpatialFilter, outputFormat );
                }
            }
        }
        else
        {
/* -------------------------------------------------------------------- */ 
/*      Process specified data source layers.                           */ 
/* -------------------------------------------------------------------- */ 
            char** papszIter = papszLayers;
            for( ; *papszIter != NULL; papszIter++ )
            {
                OGRLayer        *poLayer = poDS->GetLayerByName(*papszIter);

                if( poLayer == NULL )
                {
                    fprintf(stderr, "FAILURE: Couldn't fetch requested layer %s!\n",
                            *papszIter );
                    exit( 1 );
                }

                if( iRepeat != 0 )
                    poLayer->ResetReading();

                ReportOnLayer( poLayer, pszWHERE, pszGeomField, poSpatialFilter, outputFormat );
            }
        }
    }

/* -------------------------------------------------------------------- */
/*      Close down.                                                     */
/* -------------------------------------------------------------------- */
end:
    CSLDestroy( papszArgv );
    CSLDestroy( papszLayers );
    CSLDestroy( papszOptions );
    OGRDataSource::DestroyDataSource( poDS );
    if (poSpatialFilter)
        OGRGeometryFactory::destroyGeometry( poSpatialFilter );

    OGRCleanupAll();

    return nRet;
}

/************************************************************************/
/*                               Usage()                                */
/************************************************************************/

static void Usage(const char* pszErrorMsg)

{
    printf( "Usage: ogrinfo [--help-general] [-ro] [-q] [-where restricted_where]\n"
            "               [-spat xmin ymin xmax ymax] [-geomfield field] [-fid fid]\n"
            "               [-sql statement] [-dialect sql_dialect] [-al] [-so] [-fields={YES/NO}]\n"
            "               [-geom={YES/NO/SUMMARY}][--formats]\n"
            "               datasource_name [layer [layer ...]]\n");

    if( pszErrorMsg != NULL )
        fprintf(stderr, "\nFAILURE: %s\n", pszErrorMsg);

    exit( 1 );
}


/************************************************************************/
/*                         ReportOnLayer_GDAL()                         */
/* Outputs the layer and feature information with the traditional GDAL  */
/*  output formatting.                                                  */
/************************************************************************/
static void ReportOnLayer_GDAL( OGRLayer * poLayer, 
                           const char* pszGeomField, 
                           OGRGeometry *poSpatialFilter )
{
    OGRFeatureDefn      *poDefn = poLayer->GetLayerDefn();

/* -------------------------------------------------------------------- */
/*      Report various overall information.                             */
/* -------------------------------------------------------------------- */
    printf( "\n" );
    
    printf( "Layer name: %s\n", poLayer->GetName() );

    if( bVerbose )
    {
        int nGeomFieldCount =
            poLayer->GetLayerDefn()->GetGeomFieldCount();
        if( nGeomFieldCount > 1 )
        {
            for(int iGeom = 0;iGeom < nGeomFieldCount; iGeom ++ )
            {
                OGRGeomFieldDefn* poGFldDefn =
                    poLayer->GetLayerDefn()->GetGeomFieldDefn(iGeom);
                printf( "Geometry (%s): %s\n", poGFldDefn->GetNameRef(), 
                    OGRGeometryTypeToName( poGFldDefn->GetType() ) );
            }
        }
        else
        {
            printf( "Geometry: %s\n", 
                    OGRGeometryTypeToName( poLayer->GetGeomType() ) );
        }
        
        printf( "Feature Count: %d\n", poLayer->GetFeatureCount() );
        
        OGREnvelope oExt;
        if( nGeomFieldCount > 1 )
        {
            for(int iGeom = 0;iGeom < nGeomFieldCount; iGeom ++ )
            {
                if (poLayer->GetExtent(iGeom, &oExt, TRUE) == OGRERR_NONE)
                {
                    OGRGeomFieldDefn* poGFldDefn =
                        poLayer->GetLayerDefn()->GetGeomFieldDefn(iGeom);
                    printf("Extent (%s): (%f, %f) - (%f, %f)\n",
                           poGFldDefn->GetNameRef(),
                           oExt.MinX, oExt.MinY, oExt.MaxX, oExt.MaxY);
                }
            }
        }
        else if (poLayer->GetExtent(&oExt, TRUE) == OGRERR_NONE)
        {
            printf("Extent: (%f, %f) - (%f, %f)\n", 
                   oExt.MinX, oExt.MinY, oExt.MaxX, oExt.MaxY);
        }

        char    *pszWKT;
        
        if( nGeomFieldCount > 1 )
        {
            for(int iGeom = 0;iGeom < nGeomFieldCount; iGeom ++ )
            {
                OGRGeomFieldDefn* poGFldDefn =
                    poLayer->GetLayerDefn()->GetGeomFieldDefn(iGeom);
                OGRSpatialReference* poSRS = poGFldDefn->GetSpatialRef();
                if( poSRS == NULL )
                    pszWKT = CPLStrdup( "(unknown)" );
                else
                {
                    poSRS->exportToPrettyWkt( &pszWKT );
                }

                printf( "SRS WKT (%s):\n%s\n",
                        poGFldDefn->GetNameRef(), pszWKT );
                CPLFree( pszWKT );
            }
        }
        else
        {
            if( poLayer->GetSpatialRef() == NULL )
                pszWKT = CPLStrdup( "(unknown)" );
            else
            {
                poLayer->GetSpatialRef()->exportToPrettyWkt( &pszWKT );
            }            

            printf( "Layer SRS WKT:\n%s\n", pszWKT );
            CPLFree( pszWKT );
        }
    
        if( strlen(poLayer->GetFIDColumn()) > 0 )
            printf( "FID Column = %s\n", 
                    poLayer->GetFIDColumn() );
    
        if( nGeomFieldCount > 1 )
        {
            for(int iGeom = 0;iGeom < nGeomFieldCount; iGeom ++ )
            {
                OGRGeomFieldDefn* poGFldDefn =
                    poLayer->GetLayerDefn()->GetGeomFieldDefn(iGeom);
                printf( "Geometry Column %d = %s\n", iGeom + 1,
                        poGFldDefn->GetNameRef() );
            }
        }
        else if( strlen(poLayer->GetGeometryColumn()) > 0 )
            printf( "Geometry Column = %s\n", 
                    poLayer->GetGeometryColumn() );

        for( int iAttr = 0; iAttr < poDefn->GetFieldCount(); iAttr++ )
        {
            OGRFieldDefn    *poField = poDefn->GetFieldDefn( iAttr );
            
            printf( "%s: %s (%d.%d)\n",
                    poField->GetNameRef(),
                    poField->GetFieldTypeName( poField->GetType() ),
                    poField->GetWidth(),
                    poField->GetPrecision() );
        }
    }

/* -------------------------------------------------------------------- */
/*      Read, and dump features.                                        */
/* -------------------------------------------------------------------- */
    OGRFeature  *poFeature = NULL;

    if( nFetchFID == OGRNullFID && !bSummaryOnly )
    {
        while( (poFeature = poLayer->GetNextFeature()) != NULL )
        {
            poFeature->DumpReadable( NULL, papszOptions );
            OGRFeature::DestroyFeature( poFeature );
        }
    }
    else if( nFetchFID != OGRNullFID )
    {
        poFeature = poLayer->GetFeature( nFetchFID );
        if( poFeature == NULL )
        {
            printf( "Unable to locate feature id %d on this layer.\n", 
                    nFetchFID );
        }
        else
        {
            poFeature->DumpReadable( NULL, papszOptions );
            OGRFeature::DestroyFeature( poFeature );
        }
    }
}


/************************************************************************/
/*                         ReportOnLayer_JSON()                         */
/* Outputs the layer and feature information in JSON, including a dump  */
/*  of the features in GeoJSON format.                                  */
/************************************************************************/
static void ReportOnLayer_JSON( OGRLayer * poLayer, 
                           const char* pszGeomField, 
                           OGRGeometry *poSpatialFilter )
{
    //OGRFeatureDefn      *poDefn = poLayer->GetLayerDefn();
    OGRSFDriver         *poDriver = NULL;

    poDriver = OGRSFDriverRegistrar::GetRegistrar()->GetDriverByName( "GeoJSON" );

    if( poDriver == NULL ) {
        // TODO: Get the standard message for this error.
        fprintf( stderr, "FAILURE: GeoJSON driver not supported in this build.\n" ); 
        exit(1);
    }

    // put the features into it using GeoJSON...

}

CPLXMLNode *DumpReadableXML( OGRFeatureDefn * poDefn, OGRFeature * poFeature )
{
    CPLXMLNode *featureNode = CPLCreateXMLNode( NULL, CXT_Element, "Feature" );

    CPLSetXMLValue( featureNode, "#id", CPLSPrintf("%ld", poFeature->GetFID()) );

    for( int iAttr = 0; iAttr < poDefn->GetFieldCount(); iAttr++ )
    {
        OGRFieldDefn    *poField = poDefn->GetFieldDefn( iAttr );
        CPLXMLNode      *attrNode = CPLCreateXMLNode( featureNode, CXT_Element, "Attr" );

        CPLSetXMLValue( attrNode, "#name", poField->GetNameRef() ); 

        if( poFeature->IsFieldSet( iAttr ) )
            CPLCreateXMLNode( attrNode, CXT_Literal, 
                                poFeature->GetFieldAsString( iAttr ) );
    }

    if( poFeature->GetStyleString() != NULL )
    {
        CPLCreateXMLElementAndValue( featureNode, "Style", poFeature->GetStyleString() );
    }

    int nGeomFieldCount = poFeature->GetGeomFieldCount();
    if( nGeomFieldCount > 0 )
    {
        for( int iField = 0; iField < nGeomFieldCount; iField++ )
        {
            OGRGeomFieldDefn    *poFDefn = poDefn->GetGeomFieldDefn(iField);
            CPLXMLNode          *attrNode = CPLCreateXMLNode( featureNode, CXT_Element, "Geometry" );
            OGRGeometry         *geoRef = poFeature->GetGeomFieldRef( iField );

            CPLSetXMLValue( attrNode, "#name", poFDefn->GetNameRef() ); 
            if( geoRef != NULL ) {
                char* wkt = NULL;
                if( geoRef->exportToWkt( &wkt ) == OGRERR_NONE) {
                    CPLCreateXMLNode( attrNode, CXT_Literal, wkt );
                    CPLFree( wkt );
                }
            }
        }
    }

    return featureNode;
}

static void ReportOnLayer_XML( OGRLayer * poLayer, 
                           const char* pszGeomField, 
                           OGRGeometry *poSpatialFilter )
{
    OGRFeatureDefn      *poDefn = poLayer->GetLayerDefn();
    CPLXMLNode          *xmlRoot = CPLCreateXMLNode( NULL, CXT_Element, "Layer" );
    CPLXMLNode          *workNode;

    CPLXMLNode          *metaNode = CPLCreateXMLNode( xmlRoot, CXT_Element, "Meta" );

    CPLCreateXMLElementAndValue( metaNode, "Name", poLayer->GetName() );

    if( bVerbose )
    {
        CPLCreateXMLElementAndValue( metaNode, "FeatureCount", 
                                        CPLSPrintf("%d", poLayer->GetFeatureCount()) );

        OGREnvelope oExt;
        char    *pszWKT;

        int nGeomFieldCount =
            poLayer->GetLayerDefn()->GetGeomFieldCount();
        for(int iGeom = 0;iGeom < nGeomFieldCount; iGeom ++ )
        {
            OGRGeomFieldDefn* poGFldDefn =
                poLayer->GetLayerDefn()->GetGeomFieldDefn(iGeom);
            OGRErr extErr = poLayer->GetExtent(iGeom, &oExt, TRUE);

            workNode = CPLCreateXMLNode( metaNode, CXT_Element, "GeometryField" );
            CPLSetXMLValue( workNode, "#name", poGFldDefn->GetNameRef() );
            CPLSetXMLValue( workNode, "#type", OGRGeometryTypeToName( poGFldDefn->GetType() ) );

            if(extErr != OGRERR_NONE)
                extErr = poLayer->GetExtent(&oExt, TRUE);
            
            if(extErr == OGRERR_NONE) {
                CPLXMLNode *extentNode = CPLCreateXMLNode( workNode, CXT_Element, "Extent" );
                CPLSetXMLValue( extentNode, "#minx", CPLSPrintf( "%f", oExt.MinX) );
                CPLSetXMLValue( extentNode, "#miny", CPLSPrintf( "%f", oExt.MinY) );
                CPLSetXMLValue( extentNode, "#maxx", CPLSPrintf( "%f", oExt.MaxX) );
                CPLSetXMLValue( extentNode, "#maxy", CPLSPrintf( "%f", oExt.MaxY) );
            }
            
            OGRSpatialReference* poSRS = poGFldDefn->GetSpatialRef();
            if( poSRS == NULL )
                pszWKT = CPLStrdup( "(unknown)" );
            else
            {
                poSRS->exportToPrettyWkt( &pszWKT );
            }
            
            CPLXMLNode *srsNode = CPLCreateXMLNode( workNode, CXT_Element, "SRS" );
            CPLCreateXMLNode( srsNode, CXT_Literal, pszWKT );
            CPLFree( pszWKT );
        }
        
        for( int iAttr = 0; iAttr < poDefn->GetFieldCount(); iAttr++ )
        {
            OGRFieldDefn    *poField = poDefn->GetFieldDefn( iAttr );
            
            workNode = CPLCreateXMLNode( metaNode, CXT_Element, "Field" );
            CPLSetXMLValue( workNode, "#name", poField->GetNameRef() );
            CPLSetXMLValue( workNode, "#type", poField->GetFieldTypeName( poField->GetType() ) );
            CPLSetXMLValue( workNode, "#width", CPLSPrintf("%d", poField->GetWidth()) );
            CPLSetXMLValue( workNode, "#precision", CPLSPrintf("%d", poField->GetPrecision()) );

        }
    }


/* -------------------------------------------------------------------- */
/*      Read, and dump features in fancy XML!                           */
/* -------------------------------------------------------------------- */
    OGRFeature  *poFeature = NULL;
    CPLXMLNode  *featuresNode = CPLCreateXMLNode( xmlRoot, CXT_Element, "Features" );

    if( nFetchFID == OGRNullFID && !bSummaryOnly )
    {
        while( (poFeature = poLayer->GetNextFeature()) != NULL )
        {
            //poFeature->DumpReadable( NULL, papszOptions );
            CPLAddXMLChild( featuresNode, DumpReadableXML( poDefn, poFeature ) );
            OGRFeature::DestroyFeature( poFeature );
        }
    }
    else if( nFetchFID != OGRNullFID )
    {
        poFeature = poLayer->GetFeature( nFetchFID );
        if( poFeature == NULL )
        {
            fprintf( stderr, "Unable to locate feature id %d on this layer.\n", 
                    nFetchFID );
        }
        else
        {
            //poFeature->DumpReadable( NULL, papszOptions );
            CPLAddXMLChild( featuresNode, DumpReadableXML( poDefn, poFeature ) );
            OGRFeature::DestroyFeature( poFeature );
        }
    }


    printf("%s\n", CPLSerializeXMLTree( xmlRoot ) );
}


/************************************************************************/
/*                           ReportOnLayer()                            */
/************************************************************************/

static void ReportOnLayer( OGRLayer * poLayer, const char *pszWHERE,
                           const char* pszGeomField, 
                           OGRGeometry *poSpatialFilter,
                           InfoOutputFormat outputFormat )

{
    OGRFeatureDefn      *poDefn = poLayer->GetLayerDefn();

/* -------------------------------------------------------------------- */
/*      Set filters if provided.                                        */
/* -------------------------------------------------------------------- */
    if( pszWHERE != NULL )
    {
        if (poLayer->SetAttributeFilter( pszWHERE ) != OGRERR_NONE )
        {
            fprintf(stderr, "FAILURE: SetAttributeFilter(%s) failed.\n", pszWHERE );
            exit(1);
        }
    }

    if( poSpatialFilter != NULL )
    {
        if( pszGeomField != NULL )
        {
            int iGeomField = poDefn->GetGeomFieldIndex(pszGeomField);
            if( iGeomField >= 0 )
                poLayer->SetSpatialFilter( iGeomField, poSpatialFilter );
            else
                fprintf(stderr, "WARNING: Cannot find geometry field %s.\n",
                       pszGeomField);
        }
        else
            poLayer->SetSpatialFilter( poSpatialFilter );
    }

    if( outputFormat == ofGDAL ) 
    {
        ReportOnLayer_GDAL( poLayer, pszGeomField, poSpatialFilter );
    } 
    else if( outputFormat == ofJSON ) 
    {
        ReportOnLayer_JSON( poLayer, pszGeomField, poSpatialFilter );
    }
    else if( outputFormat == ofXML )
    {
        ReportOnLayer_XML( poLayer, pszGeomField, poSpatialFilter );
    }
}
