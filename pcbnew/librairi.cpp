/****************************************/
/* Manage module (footprint) libraries. */
/****************************************/

#include "fctsys.h"
#include "appl_wxstruct.h"
#include "class_drawpanel.h"
#include "confirm.h"
#include "kicad_string.h"
#include "gestfich.h"
#include "pcbnew.h"
#include "wxPcbStruct.h"
#include "module_editor_frame.h"
#include "dialog_helpers.h"
#include "richio.h"
#include "filter_reader.h"
#include "class_footprint_library.h"

/*
 * Module library header format:
 * Array LIBRARY HEADER-datetime
 * $INDEX
 * List of modules names (1 name per line)
 * $EndIndex
 * List of descriptions of Modules
 * $EndLIBRARY
 */
#define BACKUP_EXT                 wxT( "bak" )
#define FILETMP_EXT                wxT( "$$$" )
#define EXPORT_IMPORT_LASTPATH_KEY wxT( "import_last_path" )

const wxString        ModExportFileExtension( wxT( "emp" ) );

static const wxString ModExportFileWildcard( _( "Kicad foot print export files (*.emp)|*.emp" ) );


MODULE* FOOTPRINT_EDIT_FRAME::Import_Module()
{
    char*     Line;
    FILE*     file;
    MODULE*   module = NULL;
    bool      Footprint_Is_GPCB_Format = false;

    wxString  LastOpenedPathForLoading;
    wxConfig* Config = wxGetApp().m_EDA_Config;

    if( Config )
        Config->Read( EXPORT_IMPORT_LASTPATH_KEY, &LastOpenedPathForLoading );

    wxFileDialog dlg( this, _( "Import Footprint Module" ),
                      LastOpenedPathForLoading, wxEmptyString,
                      ModExportFileWildcard, wxFD_OPEN | wxFD_FILE_MUST_EXIST );

    if( dlg.ShowModal() == wxID_CANCEL )
        return NULL;

    file = wxFopen( dlg.GetPath(), wxT( "rt" ) );

    if( file == NULL )
    {
        wxString msg;
        msg.Printf( _( "File <%s> not found" ), GetChars( dlg.GetPath() ) );
        DisplayError( this, msg );
        return NULL;
    }

    FILE_LINE_READER fileReader( file, dlg.GetPath() );

    FILTER_READER reader( fileReader );

    if( Config )    // Save file path
    {
        LastOpenedPathForLoading = wxPathOnly( dlg.GetPath() );
        Config->Write( EXPORT_IMPORT_LASTPATH_KEY, LastOpenedPathForLoading );
    }

    // Switch the locale to standard C (needed to print floating point numbers like 1.3)
    SetLocaleTo_C_standard();

    /* Read header and test file type */
    reader.ReadLine();
    Line = reader.Line();

    if( strnicmp( Line, ENTETE_LIBRAIRIE, L_ENTETE_LIB ) != 0 )
    {
        if( strnicmp( Line, "Element", 7 ) == 0 )
        {
            Footprint_Is_GPCB_Format = true;
        }
        else
        {
            DisplayError( this, _( "Not a module file" ) );
            return NULL;
        }
    }

    /* Read file: Search the description starting line (skip lib header)*/
    if( !Footprint_Is_GPCB_Format )
    {
        while( reader.ReadLine() )
        {
            if( strnicmp( Line, "$MODULE", 7 ) == 0 )
                break;
        }
    }

    module = new MODULE( GetBoard() );

    if( Footprint_Is_GPCB_Format )
    {
        module->Read_GPCB_Descr( dlg.GetPath() );
    }
    else
    {
        module->ReadDescr( &reader );
    }

    SetLocaleTo_Default();       // revert to the current locale

    /* Insert footprint in list*/
    GetBoard()->Add( module );

    /* Display info : */
    module->DisplayInfo( this );
    Place_Module( module, NULL );
    GetBoard()->m_Status_Pcb = 0;
    GetBoard()->m_NetInfo->BuildListOfNets();

    return module;
}


void FOOTPRINT_EDIT_FRAME::Export_Module( MODULE* aModule, bool aCreateSysLib )
{
    wxFileName fn;
    FILE*      file;
    wxString   msg, path, title, wildcard;
    wxConfig*  Config = wxGetApp().m_EDA_Config;

    if( aModule == NULL )
        return;

    fn.SetName( aModule->m_LibRef );
    fn.SetExt( aCreateSysLib ? ModuleFileExtension : ModExportFileExtension );

    if( aCreateSysLib )
        path = wxGetApp().ReturnLastVisitedLibraryPath();
    else if( Config )
        Config->Read( EXPORT_IMPORT_LASTPATH_KEY, &path );

    fn.SetPath( path );
    title    = aCreateSysLib ? _( "Create New Library" ) : _( "Export Module" );
    wildcard = aCreateSysLib ?  ModuleFileWildcard : ModExportFileWildcard;
    wxFileDialog dlg( this, msg, fn.GetPath(), fn.GetFullName(), wildcard,
                      wxFD_SAVE | wxFD_OVERWRITE_PROMPT );

    if( dlg.ShowModal() == wxID_CANCEL )
        return;

    fn = dlg.GetPath();
    wxGetApp().SaveLastVisitedLibraryPath( fn.GetPath() );

    if( ( file = wxFopen( fn.GetFullPath(), wxT( "wt" ) ) ) == NULL )
    {
        msg.Printf( _( "Unable to create <%s>" ), GetChars( fn.GetFullPath() ) );
        DisplayError( this, msg );
        return;
    }

    if( !aCreateSysLib && Config )  // Save file path
    {
        Config->Write( EXPORT_IMPORT_LASTPATH_KEY, fn.GetPath() );
    }

    // Switch the locale to standard C (needed to read/write floating point numbers like 1.3)
    SetLocaleTo_C_standard();

    FOOTPRINT_LIBRARY libexport( file );
    libexport.WriteHeader();
    libexport.m_List.Add(aModule->m_LibRef);
    libexport.WriteSectionIndex();

    GetBoard()->m_Modules->Save( file );

    libexport.WriteEndOfFile();
    fclose( file );

    SetLocaleTo_Default();       // revert to the current locale

    msg.Printf( _( "Module exported in file <%s>" ), GetChars( fn.GetFullPath() ) );
    DisplayInfoMessage( this, msg );
}


void FOOTPRINT_EDIT_FRAME::Delete_Module_In_Library( const wxString& aLibname )
{
    wxFileName newFileName;
    wxFileName oldFileName;
    int        LineNum = 0;
    char       Line[1024], Name[256];
    FILE*      out_file, * lib_module;
    wxString   CmpName, msg;

    CmpName = Select_1_Module_From_List( this, aLibname, wxEmptyString, wxEmptyString );

    if( CmpName == wxEmptyString )
        return;

    /* Confirmation */
    msg.Printf( _( "Ok to delete module %s in library %s" ),
                GetChars( CmpName ), GetChars( aLibname ) );

    if( !IsOK( this, msg ) )
        return;

    oldFileName = aLibname;

    if( ( lib_module = wxFopen( oldFileName.GetFullPath(), wxT( "rt" ) ) )  == NULL )
    {
        wxString msg;
        msg.Printf( _( "Library %s not found" ), GetChars(oldFileName.GetFullPath() ) );
        DisplayError( this, msg );
        return;
    }


    FOOTPRINT_LIBRARY input_lib( lib_module );

    /* Read header. */
    if( ! input_lib.IsLibrary() )
    {
        fclose( lib_module );
        wxString msg;
        msg.Printf( _( "%s is not a Library file" ), GetChars(oldFileName.GetFullPath()) );
        DisplayError( this, msg );
        return;
    }

    /* Read module names.  */
    input_lib.RebuildIndex();
    bool found = input_lib.FindInList( CmpName );

    if( !found )
    {
        fclose( lib_module );
        msg.Printf( _( "Module [%s] not found" ), GetChars( CmpName ) );
        DisplayError( this, msg );
        return;
    }

    /* Create new library. */
    newFileName = oldFileName;
    newFileName.SetExt( FILETMP_EXT );

    if( ( out_file = wxFopen( newFileName.GetFullPath(), wxT( "wt" ) ) ) == NULL )
    {
        fclose( lib_module );
        wxString msg;
        msg = _( "Unable to create " ) + newFileName.GetFullPath();
        DisplayError( this, msg );
        return;
    }

    wxBeginBusyCursor();

    FOOTPRINT_LIBRARY output_lib( out_file );
    output_lib.m_List = input_lib.m_List;

    output_lib.WriteHeader();
    output_lib.RemoveFromList( CmpName );
    output_lib.SortList();
    output_lib.WriteSectionIndex();

    /* Copy modules. */
    rewind( lib_module );
    LineNum = input_lib.m_LineNum;

    bool copylines = false;
    while( GetLine( lib_module, Line, &LineNum ) )
    {
        StrPurge( Line );

        if( strnicmp( Line, "$MODULE", 7 ) == 0 )
        {
            copylines = true;
            sscanf( Line + 7, " %s", Name );
            msg = FROM_UTF8( Name );

            if( msg.CmpNoCase( CmpName ) == 0 )
            {
                /* Delete old module (i.e. do not copy description to out_file). */
                while( GetLine( lib_module, Line, &LineNum ) )
                {
                    if( strnicmp( Line, "$EndMODULE", 9 ) == 0 )
                        break;
                }

                continue;
            }
        }

        if( copylines )
            fprintf( out_file, "%s\n", Line );
    }

    fclose( lib_module );
    fclose( out_file );

    wxEndBusyCursor();

    /* The old library file is renamed .bak */
    wxFileName backupFileName = oldFileName;
    backupFileName.SetExt( BACKUP_EXT );

    if( backupFileName.FileExists() )
        wxRemoveFile( backupFileName.GetFullPath() );

    if( !wxRenameFile( oldFileName.GetFullPath(), backupFileName.GetFullPath() ) )
    {
        DisplayError( this, wxT( "Librairi.cpp: rename .bak err" ) );
        return;
    }

    /* The temporary file is renamed as the previous library. */
    if( !wxRenameFile( newFileName.GetFullPath(), oldFileName.GetFullPath() ) )
    {
        DisplayError( this, wxT( "Librairi.cpp: rename err 2" ) );
        return;
    }

    msg.Printf( _( "Component %s deleted in library %s" ), GetChars( CmpName ),
                GetChars( oldFileName.GetFullPath() ) );
    SetStatusText( msg );
}


void PCB_BASE_FRAME::Archive_Modules( const wxString& LibName, bool NewModulesOnly )
{
    int      ii, NbModules = 0;
    MODULE*  Module;
    wxString fileName = LibName, path;

    if( GetBoard()->m_Modules == NULL )
    {
        DisplayInfoMessage( this, _( " No modules to archive!" ) );
        return;
    }

    path = wxGetApp().ReturnLastVisitedLibraryPath();

    if( LibName.IsEmpty() )
    {
        wxFileDialog dlg( this, _( "Library" ), path,
                          wxEmptyString, ModuleFileWildcard,
                          wxFD_SAVE | wxFD_OVERWRITE_PROMPT );

        if( dlg.ShowModal() == wxID_CANCEL )
            return;

        fileName = dlg.GetPath();
    }

    wxFileName fn( fileName );
    wxGetApp().SaveLastVisitedLibraryPath( fn.GetPath() );
    bool       file_exists = wxFileExists( fileName );

    if( !NewModulesOnly && file_exists )
    {
        wxString msg;
        msg.Printf( _( "File %s exists, OK to replace ?" ), GetChars( fileName ) );

        if( !IsOK( this, msg ) )
            return;
    }

    DrawPanel->m_AbortRequest = false;

    // Create a new, empty library if no old lib, or if archive all modules
    if( !NewModulesOnly || !file_exists )
    {
        FILE* lib_module;

        if( ( lib_module = wxFopen( fileName, wxT( "w+t" ) ) )  == NULL )
        {
            wxString msg = _( "Unable to create " ) + fileName;
            DisplayError( this, msg );
            return;
        }

        FOOTPRINT_LIBRARY new_lib( lib_module );
        new_lib.WriteHeader();
        new_lib.WriteSectionIndex();
        new_lib.WriteEndOfFile();
        fclose( lib_module );
    }

    /* Calculate the number of modules. */
    Module = (MODULE*) GetBoard()->m_Modules;

    for( ; Module != NULL; Module = (MODULE*) Module->Next() )
        NbModules++;

    double step = 100.0 / NbModules;
    DisplayActivity( 0, wxEmptyString );

    Module = (MODULE*) GetBoard()->m_Modules;

    for( ii = 1; Module != NULL; ii++, Module = (MODULE*) Module->Next() )
    {
        if( Save_Module_In_Library( fileName, Module,
                                    NewModulesOnly ? false : true,
                                    false ) == 0 )
            break;
        DisplayActivity( (int) ( ii * step ), wxEmptyString );

        /* Check for request to stop backup (ESCAPE key actuated) */
        if( DrawPanel->m_AbortRequest )
            break;
    }
}


bool PCB_BASE_FRAME::Save_Module_In_Library( const wxString& aLibName,
                                             MODULE*         aModule,
                                             bool            aOverwrite,
                                             bool            aDisplayDialog )
{
    wxFileName oldFileName;
    wxFileName newFileName;
    int        LineNum = 0, tmp;
    char       Name[256], Line[1024];
    wxString   Name_Cmp;
    wxString   msg;
    FILE*      lib_module, * dest;

    if( aModule == NULL )
        return false;

    aModule->DisplayInfo( this );

    newFileName = aLibName;

    if( !newFileName.FileExists( aLibName ) )
    {
        msg.Printf( _( "Library %s not found." ), GetChars( aLibName ) );
        DisplayError( this, msg );
        return false;
    }

    if( !IsWritable( newFileName ) )
        return false;

    /* Ask for the footprint name in lib */
    Name_Cmp = aModule->m_LibRef;

    if( aDisplayDialog )
    {
        wxTextEntryDialog dlg( this, _( "Name:" ), _( "Save module" ), Name_Cmp );

        if( dlg.ShowModal() != wxID_OK )
            return false; // cancelled by user

        Name_Cmp = dlg.GetValue();
        Name_Cmp.Trim( true );
        Name_Cmp.Trim( false );

        if( Name_Cmp.IsEmpty() )
            return false;

        aModule->m_LibRef = Name_Cmp;
    }

    // Ensure this footprint has a libname
    if( Name_Cmp.IsEmpty() )
    {
        Name_Cmp = wxT("noname");
        aModule->m_LibRef = Name_Cmp;
    }

    if( ( lib_module = wxFopen( aLibName, wxT( "rt" ) ) ) == NULL )
    {
        msg.Printf( _( "Unable to open %s" ), GetChars( aLibName ) );
        DisplayError( this, msg );
        return false;
    }

    /* Read library file */
    FOOTPRINT_LIBRARY input_lib( lib_module );

    if( ! input_lib.IsLibrary() )
    {
        fclose( lib_module );
        msg.Printf( _( "File %s is not a eeschema library" ), GetChars( aLibName ) );
        DisplayError( this, msg );
        return false;
    }

    /* Read footprints in lib: - search for an existing footprint */
    input_lib.RebuildIndex();
    bool module_exists = input_lib.FindInList( Name_Cmp );

    if( module_exists )
    {
        // an existing footprint is found in current lib
        if( aDisplayDialog )
        {
            msg = _( "Module exists\n Line: " );
            msg << LineNum;
            SetStatusText( msg );
        }

        if( !aOverwrite )    // Do not save the given footprint: an old one exists
        {
            fclose( lib_module );
            return true;
        }
    }

    /* Creates the new library */

    newFileName.SetExt( FILETMP_EXT );

    if( ( dest = wxFopen( newFileName.GetFullPath(), wxT( "w+t" ) ) )  == NULL )
    {
        fclose( lib_module );
        msg = _( "Unable to create " ) + newFileName.GetFullPath();
        DisplayError( this, msg );
        return false;
    }

    wxBeginBusyCursor();

    // Switch the locale to standard C (needed to print floating point numbers like 1.3)
    SetLocaleTo_C_standard();

    FOOTPRINT_LIBRARY output_lib( dest );
    output_lib.m_List = input_lib.m_List;

    if( ! module_exists )
        output_lib.m_List.Add( Name_Cmp );

    output_lib.SortList();

    /* Create the library header with a new date */
    output_lib.WriteHeader();
    output_lib.WriteSectionIndex();

    LineNum = 0;
    rewind( lib_module);

    // Copy footprints, until the old footprint to delete
    bool skip_header = true;

    while( GetLine( lib_module, Line, &LineNum ) )
    {
        StrPurge( Line );
        if( strnicmp( Line, "$EndLIBRARY", 8 ) == 0 )
            continue;

        // Search fo the beginning of module section:
        if( skip_header )
        {
            if(  strnicmp( Line, "$MODULE", 7 ) == 0 )
                skip_header = false;
            else
                continue;
        }

        if( strnicmp( Line, "$MODULE", 7 ) == 0 )
        {
            sscanf( Line + 7, " %s", Name );
            msg = FROM_UTF8( Name );

            if( msg.CmpNoCase( Name_Cmp ) == 0 )
            {
                /* skip old footprint descr (delete from the lib) */
                while( GetLine( lib_module, Line, &LineNum ) )
                {
                    if( strnicmp( Line, "$EndMODULE", 9 ) == 0 )
                        break;
                }

                continue;
            }
        }

        fprintf( dest, "%s\n", Line );
    }

    /* Write the new footprint ( append it to the list of footprint ) */
    tmp = aModule->m_TimeStamp; aModule->m_TimeStamp = 0;
    aModule->Save( dest );
    aModule->m_TimeStamp = tmp;

    output_lib.WriteEndOfFile();

    fclose( dest );
    fclose( lib_module );
    SetLocaleTo_Default();       // revert to the current locale

    wxEndBusyCursor();

    /* The old library file is renamed .bak */
    oldFileName = aLibName;
    oldFileName.SetExt( BACKUP_EXT );

    if( oldFileName.FileExists() )
        wxRemoveFile( oldFileName.GetFullPath() );

    if( !wxRenameFile( aLibName, oldFileName.GetFullPath() ) )
        DisplayError( this, wxT( "Librairi.cpp: rename .bak err" ) );

    /* The new library file is renamed */
    if( !wxRenameFile( newFileName.GetFullPath(), aLibName ) )
    {
        DisplayError( this, wxT( "Librairi.cpp: rename NewLib err" ) );
        return false;
    }

    if( aDisplayDialog )
    {
        msg  = _( "Component " ); msg += Name_Cmp;
        msg += module_exists ? _( " replaced in " ) : _( " added in " );
        msg += aLibName;
        SetStatusText( msg );
    }

    return true;
}


MODULE* PCB_BASE_FRAME::Create_1_Module( const wxString& aModuleName )
{
    MODULE*  Module;
    wxString moduleName;
    wxPoint  newpos;

    moduleName = aModuleName;

    /* Ask for the new module reference */
    if( moduleName.IsEmpty() )
    {
        wxTextEntryDialog dlg( this, _( "Module Reference:" ),
                               _( "Module Creation" ), moduleName );

        if( dlg.ShowModal() != wxID_OK )
            return NULL;    //Aborted by user

        moduleName = dlg.GetValue();
    }
    moduleName.Trim( true );
    moduleName.Trim( false );

    if( moduleName.IsEmpty( ) )
    {
        DisplayInfoMessage( this, _( "No reference, aborted" ) );
        return NULL;
    }

    // Creates the new module and add it to the head of the linked list of modules
    Module = new MODULE( GetBoard() );

    GetBoard()->Add( Module );

    /* Update parameters: position, timestamp ... */
    newpos = GetScreen()->GetCrossHairPosition();
    Module->SetPosition( newpos );
    Module->m_LastEdit_Time = time( NULL );

    /* Update its name in lib */
    Module->m_LibRef = moduleName;

    /* Update reference: */
    Module->m_Reference->m_Text = moduleName;
    Module->m_Reference->SetThickness( g_ModuleTextWidth );
    Module->m_Reference->SetSize( g_ModuleTextSize );

    /* Set the value field to a default value */
    Module->m_Value->m_Text = wxT( "VAL**" );
    Module->m_Value->SetThickness( g_ModuleTextWidth );
    Module->m_Value->SetSize( g_ModuleTextSize );

    Module->SetPosition( wxPoint( 0, 0 ) );

    Module->DisplayInfo( this );
    return Module;
}


void FOOTPRINT_EDIT_FRAME::Select_Active_Library()
{
    wxString msg;

    if( g_LibName_List.GetCount() == 0 )
        return;

    WinEDAListBox dlg( this, _( "Select Active Library:" ), g_LibName_List, m_CurrentLib );

    if( dlg.ShowModal() != wxID_OK )
        return;

    wxFileName fileName = wxFileName( wxEmptyString, dlg.GetTextSelection(), ModuleFileExtension );
    fileName = wxGetApp().FindLibraryPath( fileName );

    if( fileName.IsOk() && fileName.FileExists() )
    {
        m_CurrentLib = dlg.GetTextSelection();
    }
    else
    {
        msg.Printf( _( "The footprint library <%s> could not be found in any of the search paths." ),
                    GetChars( dlg.GetTextSelection() ) );
        DisplayError( this, msg );
        m_CurrentLib.Empty();
    }

    UpdateTitle();
}


int FOOTPRINT_EDIT_FRAME::Create_Librairie( const wxString& aLibName )
{
    FILE*    lib_module;
    wxString msg;
    wxFileName fileName = aLibName;

    if( fileName.FileExists() )
    {
        msg.Printf( _( "Library <%s> already exists." ), GetChars( fileName.GetFullPath() ) );
        DisplayError( this, msg );
        return 0;
    }

    if( !IsWritable( fileName ) )
        return 0;

    if( ( lib_module = wxFopen( fileName.GetFullPath(), wxT( "wt" ) ) )  == NULL )
    {
        msg = _( "Unable to create " ) + fileName.GetFullPath();
        DisplayError( this, msg );
        return -1;
    }

    FOOTPRINT_LIBRARY new_lib( lib_module );
    new_lib.WriteHeader();
    new_lib.WriteSectionIndex();
    new_lib.WriteEndOfFile();
    fclose( lib_module );

    return 1;
}
