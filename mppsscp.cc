
#include "mppsscp.h"

#include "dcmtk/oflog/oflog.h"
#include "dcmtk/ofstd/ofstd.h"
#include <dcmtk/dcmnet/assoc.h>

#define OFFIS_CONSOLE_APPLICATION "mppsscp"

#define APPLICATIONTITLE "MPPSSCP"     /* our application entity title */

#define SHORTCOL 4
#define LONGCOL 21

static OFLogger logger = OFLog::getLogger("dcmtk.apps." OFFIS_CONSOLE_APPLICATION);

static DUL_PRESENTATIONCONTEXT* findPresentationContextID(LST_HEAD *head,
                                                          T_ASC_PresentationContextID presentationContextID)
{
  DUL_PRESENTATIONCONTEXT *pc;
  LST_HEAD **l;
  OFBool found = OFFalse;

  if (head == NULL)
    return NULL;

  l = &head;
  if (*l == NULL)
    return NULL;

  pc = (DUL_PRESENTATIONCONTEXT*) LST_Head(l);
  (void)LST_Position(l, (LST_NODE*)pc);

  while (pc && !found)
  {
    if (pc->presentationContextID == presentationContextID)
    {
        found = OFTrue;
    } else
    {
        pc = (DUL_PRESENTATIONCONTEXT*) LST_Next(l);
    }
  }
  return pc;
}

// ----------------------------------------------------------------------------

static void getPresentationContextInfo(const T_ASC_Association *assoc,
                                       const Uint8 presID,
                                       DcmPresentationContextInfo &info)
{
  if (assoc == NULL)
    return;

  DUL_PRESENTATIONCONTEXT *pc = findPresentationContextID(assoc->params->DULparams.acceptedPresentationContext, presID);
  if (pc != NULL)
  {
    info.abstractSyntax = pc->abstractSyntax;
    info.acceptedTransferSyntax = pc->acceptedTransferSyntax;
    info.presentationContextID = pc->presentationContextID;
    info.proposedSCRole = pc->proposedSCRole;
    info.acceptedSCRole = pc->acceptedSCRole;
  }
  return;
}

//
// DcmMppsSCP
//
DcmMppsSCP::DcmMppsSCP() : 
  m_assoc(NULL),
  m_assocConfig(NULL),
  m_assocCfgProfileName("DEFAULT"),
  m_port(4020),
  m_aetitle("MPPSSCP"),
  m_maxReceivePDULength(ASC_DEFAULTMAXPDU),
  m_dimseTimeout(0),
  m_acseTimeout(30)
{

}

DcmMppsSCP::~DcmMppsSCP() 
{
  // Clean up memory of association configuration
  if (m_assocConfig)
  {
    delete m_assocConfig;
    m_assocConfig = NULL;
  }

  // If there is an open association, drop it and free memory (just to be sure...)
  if (m_assoc)
  {
    notifyAssociationTermination();
    ASC_dropAssociation( m_assoc );
    ASC_destroyAssociation( &m_assoc );
  }

}
// ----------------------------------------------------------------------------

void DcmMppsSCP::setPort(const Uint16 port)
{
  m_port = port;
}


// ----------------------------------------------------------------------------

void DcmMppsSCP::setAETitle(const OFString &aetitle)
{
  m_aetitle = aetitle;
}


// ----------------------------------------------------------------------------

// Reads association configuration from config file
OFCondition DcmMppsSCP::loadAssociationCfgFile(const OFString &assocFile)
{
  // delete any previous association configuration
  if (m_assocConfig)
    delete m_assocConfig;

  OFString profileName;
  m_assocConfig = new DcmAssociationConfiguration();
  OFLOG_DEBUG(logger,"Loading SCP configuration file...");
  OFCondition result = DcmAssociationConfigurationFile::initialize(*m_assocConfig, assocFile.c_str());
  if (result.bad())
  {
    OFLOG_ERROR(logger,"DcmMppsSCP: Unable to parse association configuration file " << assocFile << ": " << result.text());
    delete m_assocConfig;
    m_assocConfig = NULL;
  }
  return result;
}

// ----------------------------------------------------------------------------

OFCondition DcmMppsSCP::listen()
{
  OFCondition cond = EC_Normal;
  // Make sure data dictionary is loaded.
  if( !dcmDataDict.isDictionaryLoaded() )
    OFLOG_WARN(logger,"no data dictionary loaded, check environment variable: " << DCM_DICT_ENVIRONMENT_VARIABLE);

  // If port is privileged we must be as well.
  if( m_port < 1024 && geteuid() != 0 )
  {
    OFLOG_ERROR(logger,"No privileges to open this network port (choose port below 1024?)");
    return EC_IllegalCall; // TODO: need to find better error code
  }

    // Initialize network, i.e. create an instance of T_ASC_Network*.
  T_ASC_Network *m_net = NULL;
  cond = ASC_initializeNetwork( NET_ACCEPTOR, OFstatic_cast(int, m_port), m_acseTimeout, &m_net );
  if( cond.bad() )
    return cond;

  // Return to normal uid so that we can't do too much damage in case
  // things go very wrong. Only works if the program is setuid root,
  // and run by another user. Running as root user may be
  // potentially disasterous if this program screws up badly.
  setuid( getuid() );

  // If we get to this point, the entire initialization process has been completed
  // successfully. Now, we want to start handling all incoming requests. Since
  // this activity is supposed to represent a server process, we do not want to
  // terminate this activity (unless indicated by the stopAfterCurrentAssociation()
  // method). Hence, create an inifinite while-loop.
  while( cond.good() && !stopAfterCurrentAssociation() )
  {
    // Wait for an association and handle the requests of
    // the calling applications correspondingly.
    cond = waitForAssociation(m_net);

  }
  // Drop the network, i.e. free memory of T_ASC_Network* structure. This call
  // is the counterpart of ASC_initializeNetwork(...) which was called above.
  cond = ASC_dropNetwork( &m_net );
  m_net = NULL;
  if( cond.bad() )
    return cond;

  // return ok
  return EC_Normal;
}

// ----------------------------------------------------------------------------

OFCondition DcmMppsSCP::waitForAssociation(T_ASC_Network *network)
{
 
  if (network == NULL)
  {
    return ASC_NULLKEY;
  }
  if (m_assoc != NULL)
  {
    return DIMSE_ILLEGALASSOCIATION;
  }
  char buf[BUFSIZ];
  Uint16 timeout;

  // Depending on if the execution is limited to one single process
  // or not we need to set the timeout value correspondingly.
  // for WIN32, child processes cannot be counted (always 0) -> timeout=1000
  timeout = 1000;

  // Listen to a socket for timeout seconds and wait for an association request
  OFCondition cond = ASC_receiveAssociation( network, &m_assoc, m_maxReceivePDULength, NULL, NULL, OFFalse, DUL_NOBLOCK, OFstatic_cast(int, timeout) );

  // just return, if timeout occured (DUL_NOASSOCIATIONREQUEST)
  // or (WIN32) if dcmnet has started a child for us, to handle this
  // association (signaled by "DULC_FORKEDCHILD") -> return to "event loop"
  if ( ( cond.code() == DULC_FORKEDCHILD ) || ( cond == DUL_NOASSOCIATIONREQUEST ) )
  {
    return EC_Normal;
  }

  // call notifier function
  DcmSCPActionType desiredAction = DCMSCP_ACTION_UNDEFINED;
  notifyAssociationRequest(*m_assoc->params, desiredAction);
  if (desiredAction != DCMSCP_ACTION_UNDEFINED)
  {
    if (desiredAction == DCMSCP_ACTION_REFUSE_ASSOCIATION)
    {
      refuseAssociation( DCMSCP_INTERNAL_ERROR );
      return EC_Normal;
    }
    else desiredAction = DCMSCP_ACTION_UNDEFINED; // reset for later use
  }

  // Now we have to figure out if we might have to refuse the association request.
  // This is the case if at least one of five conditions is met:

  // Condition 2: determine the application context name. If an error occurred or if the
  // application context name is not supported we want to refuse the association request.
  cond = ASC_getApplicationContextName( m_assoc->params, buf );
  if( cond.bad() || strcmp( buf, DICOM_STDAPPLICATIONCONTEXT ) != 0 )
  {
    refuseAssociation( DCMSCP_BAD_APPLICATION_CONTEXT_NAME );
    return EC_Normal;
  }


  // Condition 4: if the called application entity title is not supported
  // whithin the data source we want to refuse the association request

  if( !calledAETitleAccepted(m_assoc->params->DULparams.callingAPTitle,
                             m_assoc->params->DULparams.calledAPTitle) )
  {
    refuseAssociation( DCMSCP_BAD_APPLICATION_ENTITY_SERVICE );
    return EC_Normal;
  }

  /* set our application entity title */
  ASC_setAPTitles(m_assoc->params, NULL, NULL, m_assoc->params->DULparams.calledAPTitle);

  /* If we get to this point the association shall be negotiated.
     Thus, for every presentation context it is checked whether
     it can be accepted. However, this is only a "dry" run, i.e. there
     is not yet sent a response message to the SCU
   */
  cond = negotiateAssociation();
  if( cond.bad() )
  {
    return EC_Normal;
  }

  // Reject association if no presentation context was negotiated
  if( ASC_countAcceptedPresentationContexts( m_assoc->params ) == 0 )
  {
    // Dump some debug information
    OFString tempStr;
    OFLOG_INFO(logger,"No Acceptable Presentation Contexts");
    OFLOG_DEBUG(logger,ASC_dumpParameters(tempStr, m_assoc->params, ASC_ASSOC_RJ));
    refuseAssociation( DCMSCP_NO_PRESENTATION_CONTEXTS );
    return EC_Normal;
  }

  // If the negotiation was successful, accept the association request
  cond = ASC_acknowledgeAssociation( m_assoc );
  if( cond.bad() )
  {
    return EC_Normal;
  }
  notifyAssociationAcknowledge();

  // Dump some debug information
  OFString tempStr;
  OFLOG_INFO(logger,"Association Acknowledged (Max Send PDV: " << OFstatic_cast(Uint32, m_assoc->sendPDVLength) << ")");
//  OFLOG_DEBUG(logger,ASC_dumpParameters(tempStr, m_assoc->params, ASC_ASSOC_AC));

  // process to handle the association or don't. (Note: For Windows dcmnet is handling
  // the creation for a new subprocess, so we can call HandleAssociation directly, too.)
    // Go ahead and handle the association (i.e. handle the callers requests) in this process
  handleAssociation();

  return EC_Normal;
}

/* ************************************************************************** */
/*                            Notify functions                                */
/* ************************************************************************** */


void DcmMppsSCP::notifyAssociationRequest(const T_ASC_Parameters &params,
                                      DcmSCPActionType & /* desiredAction */)
{
  // Dump some information if required
  OFLOG_INFO(logger,"Association Received " << params.DULparams.callingPresentationAddress << ": "
                                      << params.DULparams.callingAPTitle << " -> "
                                      << params.DULparams.calledAPTitle);

    // Dump more information if required
  OFString tempStr;
  OFLOG_DEBUG(logger,"Incoming Association Request:" << OFendl << ASC_dumpParameters(tempStr, m_assoc->params, ASC_ASSOC_RQ));
}


// ----------------------------------------------------------------------------

OFCondition DcmMppsSCP::negotiateAssociation()
{
  // check whether there is something to negotiate...
  if (m_assoc == NULL)
    return DIMSE_ILLEGALASSOCIATION;

  if (m_assocConfig == NULL)
  {
    OFLOG_ERROR(logger,"Cannot negotiate association: Missing association configuration");
    return EC_IllegalCall; // TODO: need to find better error code
  }

  /* set presentation contexts as defined in config file */
  OFCondition result;
  if (m_assocConfig)
    result = m_assocConfig->evaluateAssociationParameters(m_assocCfgProfileName.c_str(), *m_assoc);
  else
    return EC_IllegalCall; // TODO: need to find better error code
  if (result.bad())
  {
    OFString tempStr;
    OFLOG_ERROR(logger,DimseCondition::dump(tempStr, result));
  }
  return result;
}


// ----------------------------------------------------------------------------

void DcmMppsSCP::handleAssociation()
{
  if (m_assoc == NULL)
  {
    OFLOG_WARN(logger,"DcmMppsSCP::handleAssociation() called but SCP actually has no association running, ignoring");
    return;
  }

  // Receive a DIMSE command and perform all the necessary actions. (Note that ReceiveAndHandleCommands()
  // will always return a value 'cond' for which 'cond.bad()' will be true. This value indicates that either
  // some kind of error occurred, or that the peer aborted the association (DUL_PEERABORTEDASSOCIATION),
  // or that the peer requested the release of the association (DUL_PEERREQUESTEDRELEASE).) (Also note
  // that ReceiveAndHandleCommands() will never return EC_Normal.)
  OFCondition cond = EC_Normal;
  T_DIMSE_Message msg;
  T_ASC_PresentationContextID presID;

  // start a loop to be able to receive more than one DIMSE command
  while( cond.good() )
  {
    // receive a DIMSE command over the network
    cond = DIMSE_receiveCommand( m_assoc, DIMSE_BLOCKING, 0, &presID, &msg, NULL );

    // check if peer did release or abort, or if we have a valid message
    if( cond.good() )
    {
      DcmPresentationContextInfo pcInfo;
      getPresentationContextInfo(m_assoc, presID, pcInfo);
      cond = handleIncomingCommand(&msg, pcInfo);
    }
  }
  // Clean up on association termination.
  if( cond == DUL_PEERREQUESTEDRELEASE )
  {
    notifyReleaseRequest();
    ASC_acknowledgeRelease(m_assoc);
    ASC_dropSCPAssociation(m_assoc);
  }
  else if( cond == DUL_PEERABORTEDASSOCIATION )
  {
    notifyAbortRequest();
  }
  else
  {
    notifyDIMSEError(cond);
    ASC_abortAssociation( m_assoc );
  }

  // Drop and destroy the association.
  if (m_assoc)
  {
    notifyAssociationTermination();
    ASC_dropAssociation( m_assoc );
    ASC_destroyAssociation( &m_assoc );
  }

  // Dump some information if required.
//  OFLOG_DEBUG( logger,"+++++++++++++++++++++++++++++" );
}

OFCondition DcmMppsSCP::handleIncomingCommand(T_DIMSE_Message *msg,
                                          const DcmPresentationContextInfo &info)
{
  OFCondition cond;
  if( msg->CommandField == DIMSE_C_ECHO_RQ )
  {
    // Process C-ECHO request
    cond = handleECHORequest( msg->msg.CEchoRQ, info.presentationContextID );
  } 
  else if ( msg->CommandField == DIMSE_N_CREATE_RQ ) 
  {
    // Process N-CREATE request
    cond = handleNCREATERequest( msg->msg.NCreateRQ, info.presentationContextID );

  }
  else if ( msg->CommandField == DIMSE_N_SET_RQ ) 
  {
    // Process N-SET request
    cond = handleNSETRequest( msg->msg.NSetRQ, info.presentationContextID );

  }
  else {
    // We cannot handle this kind of message. Note that the condition will be returned
    // and that the caller is responsible to end the association if desired.
    OFString tempStr;
    OFLOG_ERROR(logger,"Cannot handle this kind of DIMSE command (0x"
      << STD_NAMESPACE hex << STD_NAMESPACE setfill('0') << STD_NAMESPACE setw(4)
      << OFstatic_cast(unsigned int, msg->CommandField) << ")");
    OFLOG_DEBUG(logger,DIMSE_dumpMessage(tempStr, *msg, DIMSE_INCOMING));
    cond = DIMSE_BADCOMMANDTYPE;
  }

  // return result
  return cond;
}

// ----------------------------------------------------------------------------

OFCondition DcmMppsSCP::handleNCREATERequest(T_DIMSE_N_CreateRQ &reqMessage,
                                      T_ASC_PresentationContextID presID)
{
    OFCondition cond = EC_Normal;
    DcmDataset *rqDataset = NULL;
    if (reqMessage.DataSetType == DIMSE_DATASET_PRESENT)
    {
        if (m_assoc != NULL) {
        cond = DIMSE_receiveDataSetInMemory(m_assoc, DIMSE_BLOCKING, 0, &presID, &rqDataset, NULL, NULL);
        if (cond.bad()) return cond;
        }
        else 
        {
            printf("m_assoc is NULL\n");
        }
    }

    OFString tempStr;
    OFLOG_INFO(logger,"Received N-CREATE Request");
    OFLOG_INFO(logger,DIMSE_dumpMessage(tempStr, reqMessage, DIMSE_INCOMING, rqDataset, presID));

    T_DIMSE_Message rsp;
    rsp.CommandField = DIMSE_N_CREATE_RSP;
    rsp.msg.NCreateRSP.MessageIDBeingRespondedTo = reqMessage.MessageID;
    rsp.msg.NCreateRSP.AffectedSOPClassUID[0] = 0;
    rsp.msg.NCreateRSP.DimseStatus = STATUS_Success;
    if (reqMessage.opts & O_NCREATE_AFFECTEDSOPINSTANCEUID)
    {
        // instance UID is provided by SCU
        strncpy(rsp.msg.NCreateRSP.AffectedSOPInstanceUID, reqMessage.AffectedSOPInstanceUID, sizeof(DIC_UI));
        rsp.msg.NCreateRSP.AffectedSOPInstanceUID[sizeof(DIC_UI)-1]= 0;
    } else {
        // we generate our own instance UID
        dcmGenerateUniqueIdentifier(rsp.msg.NCreateRSP.AffectedSOPInstanceUID);
    }
    rsp.msg.NCreateRSP.DataSetType = DIMSE_DATASET_NULL;

    OFString sopClass(reqMessage.AffectedSOPClassUID);
    if (sopClass != UID_ModalityPerformedProcedureStepSOPClass)
    {
        OFLOG_ERROR(logger,"N-CREATE unsupported for SOP class '" << sopClass << "'");
        rsp.msg.NCreateRSP.DimseStatus = STATUS_N_NoSuchSOPClass;
        rsp.msg.NCreateRSP.opts = 0;  // don't include affected SOP instance UID
    }

    DcmDataset *rspDataset = NULL;
    OFLOG_INFO(logger,"Sent N-CREATE Response");
    OFLOG_DEBUG(logger,DIMSE_dumpMessage(tempStr, rsp, DIMSE_OUTGOING, rspDataset, presID));

    DcmDataset *rspCommand = NULL;
    cond = DIMSE_sendMessageUsingMemoryData(m_assoc, presID, &rsp, NULL, rspDataset, NULL, NULL, &rspCommand);
    if (cond.bad()) {
        OFLOG_ERROR(logger,"Failed sending N-CREATE response: " << DimseCondition::dump(tempStr, cond));
    }

    delete rspCommand;
    delete rqDataset;
    delete rspDataset;

    // return result
    return cond;
}


// ----------------------------------------------------------------------------

OFCondition DcmMppsSCP::handleNSETRequest(T_DIMSE_N_SetRQ &reqMessage,
                                      T_ASC_PresentationContextID presID)
{ 
    OFCondition cond = EC_Normal;
    DcmDataset *rqDataset = NULL;

    if (reqMessage.DataSetType == DIMSE_DATASET_PRESENT)
    {
        cond = DIMSE_receiveDataSetInMemory(m_assoc, DIMSE_BLOCKING, 0, &presID, &rqDataset, NULL, NULL);
        if (cond.bad()) return cond;
    }

    OFString tempStr;
    OFLOG_INFO(logger, "Received N-SET Request");
    OFLOG_INFO(logger, DIMSE_dumpMessage(tempStr, reqMessage, DIMSE_INCOMING, rqDataset, presID));

    // initialize response message
    T_DIMSE_Message rsp;
    rsp.CommandField = DIMSE_N_SET_RSP;
    rsp.msg.NSetRSP.MessageIDBeingRespondedTo = reqMessage.MessageID;
    rsp.msg.NSetRSP.AffectedSOPClassUID[0] = 0;
    rsp.msg.NSetRSP.DimseStatus = STATUS_Success;
    rsp.msg.NSetRSP.AffectedSOPInstanceUID[0] = 0;
    rsp.msg.NSetRSP.DataSetType = DIMSE_DATASET_NULL;
    rsp.msg.NSetRSP.opts = 0;

    OFString sopClass(reqMessage.RequestedSOPClassUID);
    if (sopClass != UID_ModalityPerformedProcedureStepSOPClass)
    {
        OFLOG_ERROR(logger, "N-SET unsupported for SOP class '" << sopClass << "'");
        rsp.msg.NSetRSP.DimseStatus = STATUS_N_NoSuchSOPClass;
    }

    DcmDataset *rspDataset = NULL;
    DcmDataset *rspCommand = NULL;
    OFLOG_INFO(logger,"Sent N-SET Response");
    OFLOG_DEBUG(logger,DIMSE_dumpMessage(tempStr, rsp, DIMSE_OUTGOING, rspDataset, presID));

    cond = DIMSE_sendMessageUsingMemoryData(m_assoc, presID, &rsp, NULL, rspDataset, NULL, NULL, &rspCommand);
    if (cond.bad()) {
        OFLOG_ERROR(logger, "Failed sending N-SET response: " << DimseCondition::dump(tempStr, cond));
    }

    delete rspCommand;
    delete rqDataset;
    delete rspDataset;
 
    // return result
    return cond;
}   



#include "mppsscp.h"
#include "dcmtk/ofstd/ofstd.h"
#include <dcmtk/dcmnet/assoc.h>

#define OFFIS_CONSOLE_APPLICATION "mppsscp"

#define APPLICATIONTITLE "MPPSSCP"     /* our application entity title */

#define SHORTCOL 4
#define LONGCOL 21
//
// main
//
OFCmdUnsignedInt   opt_port = 0;
const char *       opt_respondingAETitle = APPLICATIONTITLE;

int main(int argc, char *argv[])
{
    OFConsoleApplication app(OFFIS_CONSOLE_APPLICATION, "DICOM mpps (N-CREATE,N-SET) SCP");
    OFCommandLine cmd;

    cmd.setParamColumn(LONGCOL + SHORTCOL + 4);
    cmd.addParam("port", "tcp/ip port number to listen on", OFCmdParam::PM_Optional);

    cmd.setOptionColumns(LONGCOL, SHORTCOL);
    cmd.addGroup("general options:", LONGCOL, SHORTCOL + 2);
    cmd.addOption("--help",                     "-h",      "print this help text and exit", OFCommandLine::AF_Exclusive);
    cmd.addOption("--version",                             "print version information and exit", OFCommandLine::AF_Exclusive);

    OFString opt1 = "set my AE title (default: ";
    opt1 += APPLICATIONTITLE;
    opt1 += ")";
    cmd.addOption("--aetitle",                "-aet", 1, "[a]etitle: string", opt1.c_str());
    OFLog::addOptions(cmd);

    /* evaluate command line */
    prepareCmdLineArgs(argc, argv, OFFIS_CONSOLE_APPLICATION);
    if (app.parseCommandLine(cmd, argc, argv, OFCommandLine::PF_ExpandWildcards))
    {
        /* print help text and exit */
        if (cmd.getArgCount() == 0)
            app.printUsage();

        if (cmd.findOption("--aetitle")) app.checkValue(cmd.getValue(opt_respondingAETitle));

        app.checkParam(cmd.getParamAndCheckMinMax(1, opt_port, 1, 65535));
        OFLog::configureFromCommandLine(cmd, app);

    }

    OFString config = "mppsscp.cfg";

    DcmMppsSCP *mppsscp = new DcmMppsSCP();
    mppsscp->setPort(opt_port);
    mppsscp->setAETitle(opt_respondingAETitle);
   
   
    OFCondition cond = EC_Normal;
    cond = mppsscp->loadAssociationCfgFile(config);
    if (cond.bad()) {
        printf ("loadAssociationCfgFile fail. setup manually\n");
        exit(-1);
    }

    mppsscp->listen();

    delete mppsscp;
}
