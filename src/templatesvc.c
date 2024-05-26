/*======================================================--======================
MIT License

Copyright (c) 2024 Trevor Monk

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
==============================================================================*/

/*!
 * @defgroup templatesvc templatesvc
 * @brief Trigger template file generation on variable changes
 * @{
 */

/*============================================================================*/
/*!
@file templatesvc.c

    Template Service

    The templatesvc Application maps trigger variables to template files
    using a JSON object definition to describe the mapping.  The templates
    are rendered when the trigger variables change.

*/
/*============================================================================*/

/*==============================================================================
        Includes
==============================================================================*/

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <syslog.h>
#include <signal.h>
#include <time.h>
#include <mqueue.h>
#include <varserver/varserver.h>
#include <varserver/vartemplate.h>
#include <varserver/varfp.h>
#include <tjson/json.h>

/*==============================================================================
        Private definitions
==============================================================================*/

/*! size for the variable rendering output buffer */
#define VARFP_SIZE                  ( 256 * 1024 )

/*! specifies the type of template */
typedef enum templateType
{
    /*! regular file or stream based template */
    TMPL_FD = 0,

    /*! message queue template */
    TMPL_MQ = 1
} TemplateType;

/*! the TriggerVar object caches a trigger variable handle and
    links trigger variables into a chain */
typedef struct triggerVar
{
    /*! variable handle */
    VAR_HANDLE hVar;

    /*! variable name */
    char *name;

    /*! pointer to the next trigger variable */
    struct triggerVar *pNext;
} TriggerVar;

/*! template component which maps trigger variables to
 *  a template file */
typedef struct template
{
    /*! pointer to the trigger variables */
    TriggerVar *pTriggers;

    /*! pointer to the template file name */
    char *templateFileName;

    /*! target destination name */
    char *target;

    /*! template type */
    TemplateType type;

    /*! output file descriptor */
    int fd;

    /*! message queue handle */
    mqd_t mq;

    /*! keep the destination open */
    bool keep_open;

    /*! append (true) or overwrite (false) */
    bool append;

    /*! pointer to the next template */
    struct template *pNext;
} Template;


/*! TemplateSvc state */
typedef struct templateSvcState
{
    /*! variable server handle */
    VARSERVER_HANDLE hVarServer;

    /*! verbose flag */
    bool verbose;

    /*! name of the TemplateSvc definition file */
    char *pFileName;

    /*! Variable Output stream */
    VarFP *pVarFP;

    /*! Variable output file descriptor */
    int varFd;

    /*! size of the template rendering buffer */
    size_t varfpSize;

    /*! pointer to the file vars list */
    Template *pTemplates;
} TemplateSvcState;

/*==============================================================================
        Private file scoped variables
==============================================================================*/

/*! TemplateSvc state object */
TemplateSvcState state;

/*==============================================================================
        Private function declarations
==============================================================================*/

void main(int argc, char **argv);
static int ProcessOptions( int argC, char *argV[], TemplateSvcState *pState );
static void usage( char *cmdname );
static int SetupVarFP( TemplateSvcState *pState );
static int SetupTemplate( JNode *pNode, void *arg );
static int PrintTemplateFD( TemplateSvcState *pState, Template *pTemplate );
static int PrintTemplateMQ( TemplateSvcState *pState, Template *pTemplate );

static void SetupTerminationHandler( void );
static void TerminationHandler( int signum, siginfo_t *info, void *ptr );

static int SetupTriggerNotifications( VARSERVER_HANDLE hVarServer,
                                      TriggerVar *pTriggerVars );

static int SetupTriggerNotification( VARSERVER_HANDLE hVarServer,
                                     TriggerVar *pTriggerVar );

static int ProcessTemplates( TemplateSvcState *pState, VAR_HANDLE hVar );

static int ProcessTemplate( TemplateSvcState *pState,
                            Template *pTemplate,
                            VAR_HANDLE hVar );


/*==============================================================================
        Private function definitions
==============================================================================*/

/*============================================================================*/
/*  main                                                                      */
/*!
    Main entry point for the templatesvc application

    The main function starts the templatesvc application

    @param[in]
        argc
            number of arguments on the command line
            (including the command itself)

    @param[in]
        argv
            array of pointers to the command line arguments

    @return none

==============================================================================*/
void main(int argc, char **argv)
{
    VARSERVER_HANDLE hVarServer = NULL;
    VAR_HANDLE hVar;
    int result;
    JNode *config;
    JArray *cfg;
    int sigval;
    int sig;
    int fd;

    /* clear the templatesvc state object */
    memset( &state, 0, sizeof( state ) );

    /* set up the default VARFP size */
    state.varfpSize = VARFP_SIZE;
    state.varFd = -1;

    if( argc < 2 )
    {
        usage( argv[0] );
        exit( 1 );
    }

    /* set up the abnormal termination handler */
    SetupTerminationHandler();

    /* process the command line options */
    ProcessOptions( argc, argv, &state );

    /* set up the rendering buffer */
    SetupVarFP( &state );

    /* process the input file */
    config = JSON_Process( state.pFileName );

    /* get the configuration array */
    cfg = (JArray *)JSON_Find( config, "config" );

    /* get a handle to the VAR server */
    state.hVarServer = VARSERVER_Open();
    if( state.hVarServer != NULL )
    {
        /* set up the file vars by iterating through the configuration array */
        JSON_Iterate( cfg, SetupTemplate, (void *)&state );

        while( 1 )
        {
            /* wait for a signal from the variable server */
            sig = VARSERVER_WaitSignal( &sigval );
            if ( sig == SIG_VAR_MODIFIED )
            {
                ProcessTemplates( &state, (VAR_HANDLE)sigval );
            }
        }

        /* close the variable server */
        if ( VARSERVER_Close( state.hVarServer ) == EOK )
        {
            state.hVarServer = NULL;
        }
    }
}

/*============================================================================*/
/*  SetupTriggers                                                             */
/*!
    Set up a triggers

    The SetupTriggers function parsers an array of variable triggers
    and adds them to a trigger list.

    @param[in]
       pNode
            pointer to a JSON node which should be an array of strings.

    @param[in]
        arg
            opaque pointer argument used for the templatesvc state object

    @retval EOK - the template was set up successfully
    @retval EINVAL - the template could not be set up

==============================================================================*/
static int SetupTriggers( JNode *pNode, void *arg )
{
    JVar *pVar;
    int result = EINVAL;
    TriggerVar *pTriggerVar;
    TriggerVar **pTriggers = (TriggerVar **)arg;

    if ( ( pNode != NULL ) && ( arg != NULL ) )
    {
        if ( pNode->type == JSON_VAR )
        {
            pVar = (JVar *)pNode;
            if ( pVar->var.type == JVARTYPE_STR )
            {
                if ( pVar->var.val.str != NULL )
                {
                    /* allocate memory for the trigger record */
                    pTriggerVar = calloc( 1, sizeof( TriggerVar ) );
                    if ( pTriggerVar != NULL )
                    {
                        /* populate the trigger record */
                        pTriggerVar->name = pVar->var.val.str;

                        /* insert the new trigger record at the head of the
                           trigger list for this template */
                        pTriggerVar->pNext = *pTriggers;
                        *pTriggers = pTriggerVar;

                        /* indicate success */
                        result = EOK;
                    }
                    else
                    {
                        /* memory allocation failure */
                        result = ENOMEM;
                    }
                }
            }
        }
    }

    return result;
}

/*============================================================================*/
/*  SetupTemplate                                                             */
/*!
    Set up a triggered template object

    The SetupTemplate function is a callback function for the JSON_Iterate
    function which sets up a triggered template from the JSON configuration.
    The template definition object is expected to look similar to the
    example below:

    {
        "trigger" : ["/sys/test/info"],
        "template" : "/usr/share/templates/test.tmpl",
        "type" : "fd",
        "target" : "/splunk",
        "keep_open" : true,
        "append" : true
    }

    @param[in]
       pNode
            pointer to the template definition node

    @param[in]
        arg
            opaque pointer argument used for the templatesvc state object

    @retval EOK - the template was set up successfully
    @retval EINVAL - the template could not be set up

==============================================================================*/
static int SetupTemplate( JNode *pNode, void *arg )
{
    TemplateSvcState *pState = (TemplateSvcState *)arg;
    JVar *pName;
    JVar *pFileName;
    char *template = NULL;
    char *target = NULL;
    char *type = NULL;
    TemplateType tt = TMPL_FD;
    bool append;
    bool keep_open;
    VARSERVER_HANDLE hVarServer;
    Template *pTemplate;
    TriggerVar *pTrigger = NULL;
    int rc;
    int result = EINVAL;

    if( pState != NULL )
    {
        /* get a handle to the VarServer */
        hVarServer = pState->hVarServer;

        template = JSON_GetStr( pNode, "template" );
        type = JSON_GetStr( pNode, "type" );
        if ( type != NULL )
        {
            if ( strcmp( type, "mq" ) == 0 )
            {
                tt = TMPL_MQ;
            }
        }

        target = JSON_GetStr( pNode, "target" );
        append = JSON_GetBool( pNode, "append" );
        keep_open = JSON_GetBool( pNode, "keep_open" );

        /* allocate memory for the template */
        pTemplate = calloc( 1, sizeof( Template ) );
        if( pTemplate != NULL )
        {
            pTemplate->templateFileName = template;
            pTemplate->append = append;
            pTemplate->keep_open = keep_open;
            pTemplate->target = target;
            pTemplate->fd = -1;
            pTemplate->type = tt;

            /* set up the triggers */
            if ( JSON_Iterate( (JArray *)JSON_Find( pNode, "trigger"),
                               SetupTriggers,
                               (void *)&(pTemplate->pTriggers) ) == EOK )
            {
                rc = SetupTriggerNotifications( pState->hVarServer,
                                                pTemplate->pTriggers );
            }


            /* insert the template definition */
            pTemplate->pNext = pState->pTemplates;
            pState->pTemplates = pTemplate;

            result = EOK;
        }
    }

    return result;
}

/*============================================================================*/
/*  SetupTriggerNotifications                                                 */
/*!
    Set up all the NOTIFY_MODIFIED trigger notifications

    The SetupTriggerNotifications function iterates through the TriggerVars
    list and sets up all the NOTIFY_MODIFIED trigger notification requests
    with the variable server.

    @param[in]
        hVarServer
            Handle to the variable server

    @param[in]
        pTriggerVars
            pointer to the TriggerVar list

    @retval EOK - all the triggers were successfully set up
    @retval ENOENT - one or more of the trigger variables were not found
    @retval EINVAL - invalid arguments

==============================================================================*/
static int SetupTriggerNotifications( VARSERVER_HANDLE hVarServer,
                                      TriggerVar *pTriggerVars )
{
    int result = EINVAL;
    TriggerVar *pTriggerVar;
    int rc;

    if ( ( hVarServer != NULL ) &&
         ( pTriggerVars != NULL ) )
    {
        pTriggerVar = pTriggerVars;
        result = EOK;

        while ( pTriggerVar != NULL )
        {
            /* set up a trigger notification */
            rc = SetupTriggerNotification( hVarServer, pTriggerVar );
            if ( rc != EOK )
            {
                result = rc;
            }

            /* move to the next trigger variable */
            pTriggerVar = pTriggerVar->pNext;
        }
    }
}

/*============================================================================*/
/*  SetupTriggerNotification                                                  */
/*!
    Set up a NOTIFY_MODIFIED trigger notification

    The SetupTriggerNotification function sets up a TriggerVar NOTIFY_MODIFIED
    trigger notification request with the variable server.

    @param[in]
        hVarServer
            Handle to the variable server

    @param[in]
        pTriggerVar
            pointer to the TriggerVar to set up

    @retval EOK - the notification was successfully set up
    @retval ENOENT - the trigger variable was not found
    @retval EINVAL - invalid arguments

==============================================================================*/
static int SetupTriggerNotification( VARSERVER_HANDLE hVarServer,
                                     TriggerVar *pTriggerVar )
{
    int result = EINVAL;
    VAR_HANDLE hVar;

    if ( ( hVarServer != NULL ) &&
         ( pTriggerVar != NULL ) )
    {
        if ( pTriggerVar->name != NULL )
        {
            /* get a handle to the trigger variable */
            pTriggerVar->hVar = VAR_FindByName( hVarServer, pTriggerVar->name );
            if ( pTriggerVar->hVar != VAR_INVALID )
            {
                /* request a MODIFIED notification on the trigger variable */
                result = VAR_Notify( hVarServer,
                                     pTriggerVar->hVar,
                                     NOTIFY_MODIFIED );
            }
            else
            {
                result = ENOENT;
                fprintf( stderr,
                         "templatesvc: Cannot find variable: %s\n",
                         pTriggerVar->name );
            }
        }
    }

    return result;
}

/*============================================================================*/
/*  SetupVarFP                                                                */
/*!
    Set up a variable output stream for rendering variables to text

    The SetupVarFP function sets up a shared memory buffer backed by an
    output stream to allow us to render templates into a memory buffer,
    so we can send them to a message queue.

    @param[in]
        pState
            pointer to the Template Service State object to initialize

    @retval EOK the template rendering buffer was created
    @retval EBADF failed to create the memory buffer
    @retval EINVAL invalid arguments

==============================================================================*/
static int SetupVarFP( TemplateSvcState *pState )
{
    int result = EINVAL;
    char varfp_name[64];
    time_t now;
    int n;
    size_t len = sizeof(varfp_name);

    if ( pState != NULL )
    {
        result = EBADF;

        /* generate a temporary name for the VarFP */
        now = time(NULL);
        n = snprintf(varfp_name, sizeof(varfp_name), "templatesvc_%ld", now );
        if ( ( n > 0 ) && ( (size_t)n < len ) )
        {
            /* open a VarFP object for printing */
            pState->pVarFP = VARFP_Open( varfp_name, pState->varfpSize );
            if ( pState->pVarFP != NULL )
            {
                /* get a file descriptor for the memory buffer */
                pState->varFd = VARFP_GetFd( pState->pVarFP );
                if ( pState->varFd != -1 )
                {
                    result = EOK;
                }
            }
        }
    }

    return result;
}

/*============================================================================*/
/*  ProcessTemplates                                                          */
/*!
    Process Templates

    The ProcessTemplates function iterates through all of the templates
    checking if the specified variable handle is a trigger for any of them.

    @param[in]
        pState
            pointer to the template service state

    @param[in]
        hVar
            variable handle to check against

    @retval EOK - the templates were successfully processed
    @retval EINVAL - invalid arguments
    @retval other - processing one or more templates failed

==============================================================================*/
static int ProcessTemplates( TemplateSvcState *pState, VAR_HANDLE hVar )
{
    Template *pTemplate;
    int result = EINVAL;
    int rc;

    if ( pState != NULL )
    {
        result = EOK;

        pTemplate = pState->pTemplates;
        while( pTemplate != NULL )
        {
            rc = ProcessTemplate( pState, pTemplate, hVar );
            if ( rc != EOK )
            {
                result = rc;
            }

            pTemplate = pTemplate->pNext;
        }
    }

    return result;
}

/*============================================================================*/
/*  ProcessTemplate                                                          */
/*!
    Process a template

    The ProcessTemplate function iterates through all of the trigger variables
    associated with the template to see if any match the specified variable
    handle.  If a match is found, the template is printed using the
    PrintTemplate function.

    @param[in]
        pState
            pointer to the template service state

    @param[in]
        pTemplate
            pointer to the template to process

    @param[in]
        hVar
            variable handle to check against

    @retval EOK - the template was successfully processed
    @retval EINVAL - invalid arguments
    @retval other - template processing failed

==============================================================================*/
static int ProcessTemplate( TemplateSvcState *pState,
                            Template *pTemplate,
                            VAR_HANDLE hVar )
{
    TriggerVar *pTriggerVar;
    int result = EINVAL;

    if ( ( pState != NULL ) &&
         ( pTemplate != NULL ) &&
         ( hVar != VAR_INVALID ) )
    {
        pTriggerVar = pTemplate->pTriggers;
        while( pTriggerVar != NULL )
        {
            if ( pTriggerVar->hVar == hVar )
            {
                switch( pTemplate->type )
                {
                    case TMPL_FD:
                        result = PrintTemplateFD( pState, pTemplate );
                        break;

                    case TMPL_MQ:
                        result = PrintTemplateMQ( pState, pTemplate );
                        break;

                    default:
                        result = ENOTSUP;
                        break;
                }
            }

            pTriggerVar = pTriggerVar->pNext;
        }
    }

    return result;
}

/*============================================================================*/
/*  PrintTemplateFD                                                           */
/*!
    Print a template to a file descriptor

    The PrintTemplateFD function renders the template file to the specified
    output stream.

    @param[in]
       pState
            pointer to the TemplateSvc state object

    @param[in]
        pTemplate
            Pointer to the template to generate

    @retval EOK - template rendered successfully
    @retval EINVAL - invalid arguments

==============================================================================*/
static int PrintTemplateFD( TemplateSvcState *pState, Template *pTemplate )
{
    int result = EINVAL;
    int fd;
    char *pTemplateFile;
    char *pTarget;
    int flags = O_WRONLY | O_CREAT;

    if ( ( pState != NULL ) &&
         ( pTemplate != NULL ) )
    {
        pTemplateFile = pTemplate->templateFileName;
        pTarget = pTemplate->target;

        result = ENOENT;

        if ( ( pTemplateFile != NULL ) &&
             ( pTarget != NULL ) )
        {
            printf("Printing template %s\n", pTemplateFile );

            /* open input template */
            fd = open( pTemplateFile, O_RDONLY );

            if ( pTemplate->fd == -1 )
            {
                if ( pTemplate->append )
                {
                    flags |= O_APPEND;
                }

                /* open output stream */
                pTemplate->fd = open( pTarget, flags );
            }

            if ( ( fd > 0 ) &&
                 ( pTemplate->fd > 0 ) )
            {
                result = TEMPLATE_FileToFile( pState->hVarServer,
                                              fd,
                                              pTemplate->fd );

                if ( pTemplate->keep_open == false )
                {
                    close( pTemplate->fd );
                    pTemplate->fd = -1;
                }
            }

            if ( fd > 0 )
            {
                close( fd );
            }
        }
    }

    return result;
}

/*============================================================================*/
/*  PrintTemplateMQ                                                           */
/*!
    Print a template to a file descriptor

    The PrintTemplateMQ function renders the template file to the VARFP
    buffer and then sends the output to the assocated message queue.

    @param[in]
       pState
            pointer to the TemplateSvc state object

    @param[in]
        pTemplate
            Pointer to the template to generate

    @retval EOK - template rendered successfully
    @retval EINVAL - invalid arguments

==============================================================================*/
static int PrintTemplateMQ( TemplateSvcState *pState, Template *pTemplate )
{
    int result = EINVAL;
    int fd;
    char *pTemplateFile;
    char *pTarget;
    int flags = O_WRONLY | O_CREAT;
    char *pData;
    size_t n;
    int rc;

    if ( ( pState != NULL ) &&
         ( pTemplate != NULL ) )
    {
        pTemplateFile = pTemplate->templateFileName;
        pTarget = pTemplate->target;

        result = ENOENT;

        if ( ( pState != NULL ) &&
             ( pState->varFd > 0 ) &&
             ( pTemplateFile != NULL ) &&
             ( pTarget != NULL ) )
        {
            printf("Printing template %s\n", pTemplateFile );

            /* open input template */
            fd = open( pTemplateFile, O_RDONLY );

            if ( fd > 0 )
            {
                lseek( pState->varFd, 0, SEEK_SET );

                result = TEMPLATE_FileToFile( pState->hVarServer,
                                              fd,
                                              pState->varFd );

                if ( result == EOK )
                {
                    if ( pTemplate->mq <= 0 )
                    {
                        pTemplate->mq = mq_open( pTemplate->target, O_WRONLY );
                    }

                    if ( pTemplate->mq > 0 )
                    {
                        /* get a handle to the output buffer */
                        pData = VARFP_GetData( pState->pVarFP );
                        if( pData != NULL )
                        {
                            /* send the messsage */
                            rc = mq_send( pTemplate->mq,
                                          pData,
                                          strlen(pData ),
                                          0 );

                            result = ( rc != 0 ) ? errno : EOK;

                            if ( ( result != EOK ) ||
                                 ( pTemplate->keep_open == false ) )
                            {
                                mq_close( pTemplate->mq );
                                pTemplate->mq = -1;
                            }
                        }
                    }
                    else
                    {
                        result = EBADF;
                    }
                }
            }

            if ( fd > 0 )
            {
                close( fd );
            }
        }
    }

    return result;
}

/*============================================================================*/
/*  usage                                                                     */
/*!
    Display the application usage

    The usage function dumps the application usage message
    to stderr.

    @param[in]
       cmdname
            pointer to the invoked command name

    @return none

==============================================================================*/
static void usage( char *cmdname )
{
    if( cmdname != NULL )
    {
        fprintf(stderr,
                "usage: %s [-v] [-s size] [-h] -f filename\n"
                " [-h] : display this help\n"
                " [-v] : verbose output\n"
                " [-s] : max message size (for mq targets)\n"
                " -f <filename> : configuration file\n",
                cmdname );
    }
}

/*============================================================================*/
/*  ProcessOptions                                                            */
/*!
    Process the command line options

    The ProcessOptions function processes the command line options and
    populates the TemplateSvcState object

    @param[in]
        argC
            number of arguments
            (including the command itself)

    @param[in]
        argv
            array of pointers to the command line arguments

    @param[in]
        pState
            pointer to the TemplateSvc state object

    @return always 0

==============================================================================*/
static int ProcessOptions( int argC, char *argV[], TemplateSvcState *pState )
{
    int c;
    int result = EINVAL;
    const char *options = "hvf:s:";

    if( ( pState != NULL ) &&
        ( argV != NULL ) )
    {
        while( ( c = getopt( argC, argV, options ) ) != -1 )
        {
            switch( c )
            {
                case 'v':
                    pState->verbose = true;
                    break;

                case 's':
                    pState->varfpSize = strtoul( optarg, NULL, 0 );
                    break;

                case 'h':
                    usage( argV[0] );
                    break;

                case 'f':
                    pState->pFileName = strdup(optarg);
                    break;

                default:
                    break;

            }
        }
    }

    return 0;
}

/*============================================================================*/
/*  SetupTerminationHandler                                                   */
/*!
    Set up an abnormal termination handler

    The SetupTerminationHandler function registers a termination handler
    function with the kernel in case of an abnormal termination of this
    process.

==============================================================================*/
static void SetupTerminationHandler( void )
{
    static struct sigaction sigact;

    memset( &sigact, 0, sizeof(sigact) );

    sigact.sa_sigaction = TerminationHandler;
    sigact.sa_flags = SA_SIGINFO;

    sigaction( SIGTERM, &sigact, NULL );
    sigaction( SIGINT, &sigact, NULL );

}

/*============================================================================*/
/*  TerminationHandler                                                        */
/*!
    Abnormal termination handler

    The TerminationHandler function will be invoked in case of an abnormal
    termination of this process.  The termination handler closes
    the connection with the variable server and cleans up its VARFP shared
    memory.

@param[in]
    signum
        The signal which caused the abnormal termination (unused)

@param[in]
    info
        pointer to a siginfo_t object (unused)

@param[in]
    ptr
        signal context information (ucontext_t) (unused)

==============================================================================*/
static void TerminationHandler( int signum, siginfo_t *info, void *ptr )
{
    if ( VARSERVER_Close( state.hVarServer ) == EOK )
    {
        state.hVarServer = NULL;
    }

    if ( state.pVarFP != NULL )
    {
        /* close the output memory buffer */
        VARFP_Close( state.pVarFP );
    }

    syslog( LOG_ERR, "Abnormal termination of templatesvc" );

    exit( 1 );
}

/*! @}
 * end of templatesvc group */
