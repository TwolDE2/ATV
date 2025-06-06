#include <lib/base/eerror.h>
#include <lib/base/estring.h>
#include <lib/base/esimpleconfig.h>
#include <lib/base/esettings.h>
#include <lib/dvb/pmt.h>
#include <lib/dvb/cahandler.h>
#include <lib/dvb/specs.h>
#include <lib/dvb/dvb.h>
#include <lib/dvb/metaparser.h>
#include <lib/dvb_ci/dvbci.h>
#include <lib/dvb/epgtransponderdatareader.h>
#include <lib/dvb/scan.h>
#include <lib/dvb_ci/dvbci_session.h>
#include <dvbsi++/ca_descriptor.h>
#include <dvbsi++/ca_program_map_section.h>
#include <dvbsi++/teletext_descriptor.h>
#include <dvbsi++/descriptor_tag.h>
#include <dvbsi++/iso639_language_descriptor.h>
#include <dvbsi++/stream_identifier_descriptor.h>
#include <dvbsi++/subtitling_descriptor.h>
#include <dvbsi++/teletext_descriptor.h>
#include <dvbsi++/video_stream_descriptor.h>
#include <dvbsi++/registration_descriptor.h>
#include <dvbsi++/simple_application_location_descriptor.h>
#include <dvbsi++/simple_application_boundary_descriptor.h>
#include <dvbsi++/transport_protocol_descriptor.h>
#include <dvbsi++/application_name_descriptor.h>
#include <dvbsi++/application_profile.h>
#include <dvbsi++/application_descriptor.h>

#define PACK_VERSION(major,minor,micro) (((major) << 16) + ((minor) << 8) + (micro))
#define UNPACK_VERSION(version,major,minor,micro) { \
        major = (version)&0xff; \
        minor = (version>>8)&0xff; \
        micro = (version>>16)&0xff; \
}

int eDVBServicePMTHandler::m_debug = -1;

eDVBServicePMTHandler::eDVBServicePMTHandler()
	:m_last_channel_state(-1), m_ca_servicePtr(0), m_dvb_scan(0), m_decode_demux_num(0xFF),
	m_no_pat_entry_delay(eTimer::create()), m_have_cached_program(false)
{
	m_use_decode_demux = 0;
	m_pmt_pid = -1;
	m_dsmcc_pid = -1;
	m_service_type = livetv;
	m_ca_disabled = false;
	m_pmt_ready = false;
	if(eDVBServicePMTHandler::m_debug < 0)
		eDVBServicePMTHandler::m_debug = eSimpleConfig::getBool("config.crash.debugDVB", false) ? 1 : 0;

	eDVBResourceManager::getInstance(m_resourceManager);
	CONNECT(m_PAT.tableReady, eDVBServicePMTHandler::PATready);
	CONNECT(m_AIT.tableReady, eDVBServicePMTHandler::AITready);
	CONNECT(m_OC.tableReady, eDVBServicePMTHandler::OCready);
	CONNECT(m_no_pat_entry_delay->timeout, eDVBServicePMTHandler::sendEventNoPatEntry);
}

eDVBServicePMTHandler::~eDVBServicePMTHandler()
{
	free();
}

void eDVBServicePMTHandler::channelStateChanged(iDVBChannel *channel)
{
	int state;
	channel->getState(state);

	if ((m_last_channel_state != iDVBChannel::state_ok)
		&& (state == iDVBChannel::state_ok))
	{
		if (!m_demux && m_channel)
		{
			if (m_pvr_demux_tmp)
			{
				m_demux = m_pvr_demux_tmp;
				m_pvr_demux_tmp = NULL;
			}
			else if (m_channel->getDemux(m_demux, (!m_use_decode_demux) ? 0 : iDVBChannel::capDecode))
				eDebug("[eDVBServicePMTHandler] Allocating %s-decoding a demux for now tuned-in channel failed.", m_use_decode_demux ? "" : "non-");
		}

		if (m_demux)
		{
			if(eDVBServicePMTHandler::m_debug)
				eDebug("[eDVBServicePMTHandler] ok ... now we start!!");
			m_have_cached_program = false;

			if (m_service && !m_service->cacheEmpty())
			{
				serviceEvent(eventNewProgramInfo);
				if (m_use_decode_demux)
				{
					if (!m_ca_servicePtr)
					{
						registerCAService();
					}
					const eServiceReferenceDVB reference = eServiceReferenceDVB(m_reference.toReferenceString());
					if (m_ca_servicePtr && !m_service->usePMT())
					{
						if(eDVBServicePMTHandler::m_debug)
							eDebug("[eDVBServicePMTHandler] create cached caPMT");
						eDVBCAHandler::getInstance()->handlePMT(reference, m_service);
					}
					else if (m_ca_servicePtr && (m_service->m_flags & eDVBService::dxIsScrambledPMT))
					{
						if(eDVBServicePMTHandler::m_debug)
							eDebug("[eDVBServicePMTHandler] create caPMT to descramble PMT");
						eDVBCAHandler::getInstance()->handlePMT(reference, m_service);
					}
				}
			}

			if (!m_service || m_service->usePMT())
			{
				if (m_pmt_pid == -1)
					m_PAT.begin(eApp, eDVBPATSpec(), m_demux);
				else
					m_PMT.begin(eApp, eDVBPMTSpec(m_pmt_pid, m_reference.getServiceID().get()), m_demux);
			}

			serviceEvent(eventTuned);
		}
	} else if ((m_last_channel_state != iDVBChannel::state_failed) &&
			(state == iDVBChannel::state_failed))
	{
		eDebug("[eDVBServicePMTHandler] tune failed.");
		serviceEvent(eventTuneFailed);
	}
}

void eDVBServicePMTHandler::channelEvent(iDVBChannel *channel, int event)
{
	switch (event)
	{
	case iDVBChannel::evtPreStart:
		serviceEvent(eventPreStart);
		break;
	case iDVBChannel::evtEOF:
		serviceEvent(eventEOF);
		break;
	case iDVBChannel::evtSOF:
		serviceEvent(eventSOF);
		break;
	case iDVBChannel::evtStopped:
		serviceEvent(eventStopped);
		break;
	default:
		break;
	}
}

void eDVBServicePMTHandler::registerCAService()
{
	int demuxes[2] = {0, 0};
	uint8_t demuxid;
	uint8_t adapterid;
	m_demux->getCADemuxID(demuxid);
	m_demux->getCAAdapterID(adapterid);
	demuxes[0] = demuxid;
	if (m_decode_demux_num != 0xFF)
		demuxes[1] = m_decode_demux_num;
	else
		demuxes[1] = demuxes[0];
	const eServiceReferenceDVB reference = eServiceReferenceDVB(m_reference.toReferenceString());
	eDVBCAHandler::getInstance()->registerService(reference, adapterid, demuxes, (int)m_service_type, m_ca_servicePtr);
}

void eDVBServicePMTHandler::PMTready(int error)
{
	if (error)
		serviceEvent(eventNoPMT);
	else
	{
		m_pmt_ready = true;
		m_have_cached_program = false;
		serviceEvent(eventNewProgramInfo);
		switch (m_service_type)
		{
		case livetv:
		case recording:
		case scrambled_recording:
		case timeshift_recording:
		case scrambled_timeshift_recording:
		case streamserver:
		case scrambled_streamserver:
		case streamclient:
			eEPGTransponderDataReader::getInstance()->PMTready(this);
			break;
		default:
			/* do not start epg caching for other types of services */
			break;
		}
		if (m_use_decode_demux)
		{
			if (!m_ca_servicePtr)
			{
				registerCAService();
			}
			if (!m_ca_disabled)
			{
				eDVBCIInterfaces::getInstance()->recheckPMTHandlers();
				eDVBCIInterfaces::getInstance()->gotPMT(this);
			}
		}
		if (m_ca_servicePtr)
		{
			ePtr<eTable<ProgramMapSection> > ptr;
			const eServiceReferenceDVB reference = eServiceReferenceDVB(m_reference.toReferenceString());
			if (!m_PMT.getCurrent(ptr))
				eDVBCAHandler::getInstance()->handlePMT(reference, ptr);
			else
				eDebug("[eDVBServicePMTHandler] cannot call buildCAPMT");
		}
		if (m_service_type == pvrDescramble)
			serviceEvent(eventStartPvrDescramble);
	}
}

void eDVBServicePMTHandler::sendEventNoPatEntry()
{
	serviceEvent(eventNoPATEntry);
	
	ePtr<iDVBFrontend> fe;
	if (!m_channel->getFrontend(fe))
	{
		eDVBFrontend *frontend = (eDVBFrontend*)&(*fe);
		frontend->checkRetune();
	}
}

void eDVBServicePMTHandler::PATready(int)
{
	if(eDVBServicePMTHandler::m_debug)
		eDebug("[eDVBServicePMTHandler] PATready");
	ePtr<eTable<ProgramAssociationSection> > ptr;
	if (!m_PAT.getCurrent(ptr))
	{
		int service_id_single = -1;
		int pmtpid_single = -1;
		int pmtpid = -1;
		int cnt=0;
		int tsid=-1;
		std::vector<ProgramAssociationSection*>::const_iterator i = ptr->getSections().begin();
		tsid = (*i)->getTableIdExtension(); // in PAT this is the transport stream id
		if(eDVBServicePMTHandler::m_debug)
			eDebug("[eDVBServicePMTHandler] PAT TSID: 0x%04x (%d)", tsid, tsid);
		for (i = ptr->getSections().begin(); pmtpid == -1 && i != ptr->getSections().end(); ++i)
		{
			const ProgramAssociationSection &pat = **i;
			ProgramAssociationConstIterator program;
			for (program = pat.getPrograms()->begin(); pmtpid == -1 && program != pat.getPrograms()->end(); ++program)
			{
				if (eServiceID((*program)->getProgramNumber()) == m_reference.getServiceID())
					pmtpid = (*program)->getProgramMapPid();
				if (++cnt == 1 && pmtpid_single == -1 && pmtpid == -1)
				{
					pmtpid_single = (*program)->getProgramMapPid();
					service_id_single = (*program)->getProgramNumber();
				}
				else
					pmtpid_single = service_id_single = -1;
			}
		}
		if (pmtpid_single != -1) // only one PAT entry .. and not valid pmtpid found
		{
			if(eDVBServicePMTHandler::m_debug)
				eDebug("[eDVBServicePMTHandler] use single pat entry!");
			m_reference.setServiceID(eServiceID(service_id_single));
			pmtpid = pmtpid_single;
		}
		if (pmtpid == -1) {
			if(eDVBServicePMTHandler::m_debug)
				eDebug("[eDVBServicePMTHandler] no PAT entry found.. start delay");
			m_no_pat_entry_delay->start(1000, true);
		}
		else {
			if(eDVBServicePMTHandler::m_debug)
				eDebug("[eDVBServicePMTHandler] use pmtpid %04x for service_id %04x", pmtpid, m_reference.getServiceID().get());
			m_no_pat_entry_delay->stop();
			m_PMT.begin(eApp, eDVBPMTSpec(pmtpid, m_reference.getServiceID().get()), m_demux);
		}
	} else
		serviceEvent(eventNoPAT);
}

static void eraseHbbTVApplications(HbbTVApplicationInfoList  *applications)
{
	if(applications->size() == 0)
		return;
	for(HbbTVApplicationInfoListConstIterator info = applications->begin() ; info != applications->end() ; ++info)
		delete(*info);
	applications->clear();
}

void saveData(int orgid, unsigned char* data, int sectionLength, bool debug)
{
	int fd = 0, rc = 0;
	char fileName[255] = {0};
	sprintf(fileName, "/tmp/ait.%d", orgid);

	if (data[6] > 0)
	{
		if(debug)
			eDebug("[eDVBServicePMTHandler] section_number %d > 0", data[6]);
		data[6] = 0;
	}
	if (data[7] > data[6])
	{
		if(debug)
			eDebug("[eDVBServicePMTHandler] last_section_number %d > section_number %d", data[7], data[6]);
		data[7] = data[6];
	}

	if((fd = open(fileName, O_RDWR|O_CREAT|O_TRUNC, 0644)) < 0)
	{
		eDebug("[eDVBServicePMTHandler] Fail to save a AIT Data.");
		return;
	}
	rc = write(fd, data, sectionLength);
	if(debug)
		eDebug("[eDVBServicePMTHandler] Save Data Len : [%d]", rc);
	close(fd);
}

void eDVBServicePMTHandler::AITready(int error)
{
	if(eDVBServicePMTHandler::m_debug)
		eDebug("[eDVBServicePMTHandler] AITready");
	ePtr<eTable<ApplicationInformationSection> > ptr;
	m_aitInfoList.clear();
	if (!m_AIT.getCurrent(ptr))
	{
        short profilecode = 0;
		int orgid = 0, appid = 0, profileVersion = 0;
		m_ApplicationName = m_HBBTVUrl = "";

		int oldHbbtv = m_HbbTVApplications.size();

		eraseHbbTVApplications(&m_HbbTVApplications);

//		memcpy(m_AITData, ptr->getBufferData(), 4096);

		int sectionLength = 0;
		for (std::vector<ApplicationInformationSection*>::const_iterator it = ptr->getSections().begin(); it != ptr->getSections().end(); ++it)
		{
			std::list<ApplicationInformation *>::const_iterator i = (*it)->getApplicationInformation()->begin();
			memcpy(m_AITData, ptr->getBufferData(), 4096);
			sectionLength = (*it)->getSectionLength() + 3;
			if(eDVBServicePMTHandler::m_debug)
				eDebug("[eDVBServicePMTHandler] Section Length : %d, Total Section Length : %d", (*it)->getSectionLength(), sectionLength);
			for (; i != (*it)->getApplicationInformation()->end(); ++i)
			{
				std::string hbbtvUrl = "", applicationName = "";
				std::string boundaryExtension = "";
				std::string TPDescPath = "", SALDescPath = "";
				int controlCode = (*i)->getApplicationControlCode();
				ApplicationIdentifier * applicationIdentifier = (ApplicationIdentifier *)(*i)->getApplicationIdentifier();
				profilecode = 0;
				orgid = applicationIdentifier->getOrganisationId();
				appid = applicationIdentifier->getApplicationId();
				if(eDVBServicePMTHandler::m_debug)
					eDebug("[eDVBServicePMTHandler] found applicaions ids >> pid : %x, orgid : %d, appid : %d", m_ait_pid, orgid, appid);
				if (controlCode == 1)
				{
					saveData(orgid, m_AITData, sectionLength, eDVBServicePMTHandler::m_debug == 1);
				}
				if (controlCode == 1 || controlCode == 2) /* 1:AUTOSTART, 2:ETC */
				{
					for (DescriptorConstIterator desc = (*i)->getDescriptors()->begin();
						desc != (*i)->getDescriptors()->end(); ++desc)
					{
						switch ((*desc)->getTag())
						{
						case APPLICATION_DESCRIPTOR:
						{
							ApplicationDescriptor* applicationDescriptor = (ApplicationDescriptor*)(*desc);
							const ApplicationProfileList* applicationProfiles = applicationDescriptor->getApplicationProfiles();
							ApplicationProfileConstIterator interactionit = applicationProfiles->begin();
							for(; interactionit != applicationProfiles->end(); ++interactionit)
							{
								profilecode = (*interactionit)->getApplicationProfile();
								profileVersion = PACK_VERSION(
									(*interactionit)->getVersionMajor(),
									(*interactionit)->getVersionMinor(),
									(*interactionit)->getVersionMicro()
								);
							}
							break;
						}
						case APPLICATION_NAME_DESCRIPTOR:
						{
							ApplicationNameDescriptor *nameDescriptor  = (ApplicationNameDescriptor*)(*desc);
							ApplicationNameConstIterator interactionit = nameDescriptor->getApplicationNames()->begin();
							for(; interactionit != nameDescriptor->getApplicationNames()->end(); ++interactionit)
							{
								applicationName = (*interactionit)->getApplicationName();
								if(applicationName.size() > 0 && !isUTF8(applicationName)) {
									applicationName = convertLatin1UTF8(applicationName);
								}
								if(controlCode == 1) {
									m_ApplicationName = applicationName;
								}
								break;
							}
							break;
						}
						case TRANSPORT_PROTOCOL_DESCRIPTOR:
						{
							TransportProtocolDescriptor *transport = (TransportProtocolDescriptor*)(*desc);
							switch (transport->getProtocolId())
							{
							case 1: /* object carousel */
								if (m_dsmcc_pid >= 0)
								{
									m_OC.begin(eApp, eDVBDSMCCDLDataSpec(m_dsmcc_pid), m_demux);
								}
								break;
							case 2: /* ip */
								break;
							case 3: /* interaction */
								{
									InterActionTransportConstIterator interactionit = transport->getInteractionTransports()->begin();
									for(; interactionit != transport->getInteractionTransports()->end(); ++interactionit)
									{
										TPDescPath = (*interactionit)->getUrlBase()->getUrl();
										break;
									}
									break;
								}
							}
							break;
						}
						case GRAPHICS_CONSTRAINTS_DESCRIPTOR:
							break;
						case SIMPLE_APPLICATION_LOCATION_DESCRIPTOR:
						{
							SimpleApplicationLocationDescriptor *applicationlocation = (SimpleApplicationLocationDescriptor*)(*desc);
							SALDescPath = applicationlocation->getInitialPath();
							break;
						}
						case APPLICATION_USAGE_DESCRIPTOR:
							break;
						case SIMPLE_APPLICATION_BOUNDARY_DESCRIPTOR:
							break;
						}
					}
					// Quick'n'dirty hack to prevent crashes because of invalid UTF-8 characters
					// The root cause is in the SimpleApplicationLocationDescriptor or the AIT is buggy
					if(SALDescPath.size() == 1)
						SALDescPath="";

					hbbtvUrl = TPDescPath + SALDescPath;
				}
				if(!hbbtvUrl.empty())
				{
					const char* uu = hbbtvUrl.c_str();
					struct aitInfo aitinfo = {};
					aitinfo.id = appid;
					aitinfo.name = applicationName;
					aitinfo.url = hbbtvUrl;
					m_aitInfoList.push_back(aitinfo);
					if(!strncmp(uu, "http://", 7) || !strncmp(uu, "dvb://", 6) || !strncmp(uu, "https://", 8))
					{
						if(controlCode == 1) m_HBBTVUrl = hbbtvUrl;
						switch(profileVersion)
						{
							case 65793:
							case 66049:
								m_HbbTVApplications.push_back(new HbbTVApplicationInfo(controlCode, orgid, appid, hbbtvUrl, applicationName, profilecode));
								break;
							case 1280:
							case 65538:
							default:
								m_HbbTVApplications.push_back(new HbbTVApplicationInfo((-1)*controlCode, orgid, appid, hbbtvUrl, applicationName, profilecode));
								break;
						}
					}
					else if (!boundaryExtension.empty())
					{
						if(boundaryExtension.at(boundaryExtension.length()-1) != '/')
						{
							boundaryExtension += "/";
						}
						boundaryExtension += hbbtvUrl;
						if(controlCode == 1) m_HBBTVUrl = boundaryExtension;
						switch(profileVersion)
						{
							case 65793:
							case 66049:
								m_HbbTVApplications.push_back(new HbbTVApplicationInfo(controlCode, orgid, appid, boundaryExtension, applicationName, profilecode));
								break;
							case 1280:
							case 65538:
							default:
								m_HbbTVApplications.push_back(new HbbTVApplicationInfo((-1)*controlCode, orgid, appid, boundaryExtension, applicationName, profilecode));
								break;
						}
					}
				}
			}
		}

		if (m_HbbTVApplications.size())
		{
			if(eDVBServicePMTHandler::m_debug)
			{
				for(HbbTVApplicationInfoListConstIterator infoiter = m_HbbTVApplications.begin() ; infoiter != m_HbbTVApplications.end() ; ++infoiter)
					eDebug("[eDVBServicePMTHandler] Found : control[%d], name[%s], url[%s]",
						(*infoiter)->m_ControlCode, (*infoiter)->m_ApplicationName.c_str(), (*infoiter)->m_HbbTVUrl.c_str());
			}
			serviceEvent(eventHBBTVInfo);
		}
		else
		{
			// reset HBBTV
			if(oldHbbtv)
				serviceEvent(eventHBBTVInfo);

			if(eDVBServicePMTHandler::m_debug)
				eDebug("[eDVBServicePMTHandler] No found anything.");
		}
	}
	/* for now, do not keep listening for table updates */
	m_AIT.stop();
}

void eDVBServicePMTHandler::OCready(int error)
{
	if(eDVBServicePMTHandler::m_debug)
		eDebug("[eDVBServicePMTHandler] OCready");
	ePtr<eTable<OCSection> > ptr;
	if (!m_OC.getCurrent(ptr))
	{
		for (std::vector<OCSection*>::const_iterator it = ptr->getSections().begin(); it != ptr->getSections().end(); ++it)
		{
			[[maybe_unused]] unsigned char* sectionData = (unsigned char*)(*it)->getData();
		}
	}
	/* for now, do not keep listening for table updates */
	m_OC.stop();
}

void eDVBServicePMTHandler::getAITApplications(std::map<int, std::string> &aitlist)
{
	for (std::vector<struct aitInfo>::iterator it = m_aitInfoList.begin(); it != m_aitInfoList.end(); ++it)
	{
		aitlist[it->id] = it->url;
	}
}

void eDVBServicePMTHandler::getCaIds(std::vector<int> &caids, std::vector<int> &ecmpids, std::vector<std::string> &ecmdatabytes)
{
	program prog;

	if (!getProgramInfo(prog))
	{
		for (std::list<program::capid_pair>::iterator it = prog.caids.begin(); it != prog.caids.end(); ++it)
		{
			caids.push_back(it->caid);
			ecmpids.push_back(it->capid);
			ecmdatabytes.push_back(it->databytes);
		}
	}
}

PyObject *eDVBServicePMTHandler::getHbbTVApplications()
{
	ePyObject ret= PyList_New(0);;
	if(m_HbbTVApplications.size())
	{
		for(HbbTVApplicationInfoListConstIterator infoiter = m_HbbTVApplications.begin() ; infoiter != m_HbbTVApplications.end() ; ++infoiter)
		{
			ePyObject tuple = PyTuple_New(6);
			PyTuple_SET_ITEM(tuple, 0, PyLong_FromLong((*infoiter)->m_ControlCode));
			PyTuple_SET_ITEM(tuple, 1, PyUnicode_FromString((*infoiter)->m_ApplicationName.c_str()));
			PyTuple_SET_ITEM(tuple, 2, PyUnicode_FromString((*infoiter)->m_HbbTVUrl.c_str()));
			PyTuple_SET_ITEM(tuple, 3, PyLong_FromLong((*infoiter)->m_OrgId));
			PyTuple_SET_ITEM(tuple, 4, PyLong_FromLong((*infoiter)->m_AppId));
			PyTuple_SET_ITEM(tuple, 5, PyLong_FromLong((*infoiter)->m_ProfileCode));
			PyList_Append(ret, tuple);
			Py_DECREF(tuple);
		}
	}
	return (PyObject*)ret;
}

int eDVBServicePMTHandler::getProgramInfo(program &program)
{
//	ePtr<eTable<ProgramMapSection> > ptr;
	struct audioMap {
		int streamType;
		eDVBService::cacheID cacheTag;
	};
	int cached_apid[eDVBService::cacheMax];
	int cached_vpid = -1;
	int cached_tpid = -1;
	int ret = -1;
	uint8_t adapter, demux;

	if (m_have_cached_program)
	{
		program = m_cached_program;
		return 0;
	}

	eDVBPMTParser::clearProgramInfo(program);

	for (int m = 0; m < eDVBService::cacheMax; m++)
		cached_apid[m] = -1;

	if ( m_service && !m_service->cacheEmpty() )
	{
		cached_vpid = m_service->getCacheEntry(eDVBService::cVPID);
		for (int m = 0; m < eDVBService::nAudioCacheTags; m++)
		{
			eDVBService::cacheID cTag = eDVBService::audioCacheTags[m];
			cached_apid[cTag] = m_service->getCacheEntry(cTag);
		}
		cached_tpid = m_service->getCacheEntry(eDVBService::cTPID);
	}

	if ( ((m_service && m_service->usePMT()) || !m_service) && eDVBPMTParser::getProgramInfo(program) >= 0)
	{
		unsigned int i;
		int first_non_mpeg = -1;
		int audio_cached = -1;
		int autoaudio[eDVBService::cacheMax];
		int autoaudio_level = 4;
		const static audioMap audioMapMain[] = {
			{ audioStream::atMPEG,  eDVBService::cMPEGAPID,  },
			{ audioStream::atAC3,   eDVBService::cAC3PID,    },
			{ audioStream::atAC4,    eDVBService::cAC4PID,   },
			{ audioStream::atDDP,   eDVBService::cDDPPID,    },
			{ audioStream::atAACHE,	eDVBService::cAACHEAPID, },
			{ audioStream::atAAC,   eDVBService::cAACAPID,   },
			{ audioStream::atDTS,   eDVBService::cDTSPID,    },
			{ audioStream::atLPCM,  eDVBService::cLPCMPID,   },
			{ audioStream::atDTSHD, eDVBService::cDTSHDPID,  },
			{ audioStream::atDRA,    eDVBService::cDRAAPID,  },
		};
		static const int nAudioMapMain = sizeof audioMapMain / sizeof audioMapMain[0];

		for (int m = 0; m < eDVBService::cacheMax; m++)
			autoaudio[m] = -1;

		std::string configvalue;
		std::vector<std::string> autoaudio_languages;
		configvalue = eSettings::audio_autoselect1;
		if (configvalue != "")
			autoaudio_languages.push_back(configvalue);
		configvalue = eSettings::audio_autoselect2;
		if (configvalue != "")
			autoaudio_languages.push_back(configvalue);
		configvalue = eSettings::audio_autoselect3;
		if (configvalue != "")
			autoaudio_languages.push_back(configvalue);
		configvalue = eSettings::audio_autoselect4;
		if (configvalue != "")
			autoaudio_languages.push_back(configvalue);

		int autosub_txt_normal = -1;
		int autosub_txt_hearing = -1;
		int autosub_dvb_normal = -1;
		int autosub_dvb_hearing = -1;
		int autosub_level =4;

		std::vector<std::string> autosub_languages;
		configvalue = eSubtitleSettings::subtitle_autoselect1;
		if (configvalue != "")
			autosub_languages.push_back(configvalue);
		configvalue = eSubtitleSettings::subtitle_autoselect2;
		if (configvalue != "")
			autosub_languages.push_back(configvalue);
		configvalue = eSubtitleSettings::subtitle_autoselect3;
		if (configvalue != "")
			autosub_languages.push_back(configvalue);
		configvalue = eSubtitleSettings::subtitle_autoselect4;
		if (configvalue != "")
			autosub_languages.push_back(configvalue);

		m_dsmcc_pid = program.dsmccPid;
		if (program.aitPid >= 0)
		{
			m_AIT.begin(eApp, eDVBAITSpec(program.aitPid), m_demux);
		}

		for (i = 0; i < program.videoStreams.size(); i++)
		{
			if (program.videoStreams[i].pid == cached_vpid)
			{
				/* put cached vpid at the front of the videoStreams vector */
				if (i > 0)
				{
					videoStream tmp = program.videoStreams[i];
					program.videoStreams[i] = program.videoStreams[0];
					program.videoStreams[0] = tmp;
				}
				break;
			}
		}
		i = 0;
		for (std::vector<audioStream>::const_iterator
			as(program.audioStreams.begin());
			as != program.audioStreams.end(); ++as, ++i)
		{
			for (int m = 0; m < eDVBService::nAudioCacheTags; m++)
			{
				eDVBService::cacheID cTag = eDVBService::audioCacheTags[m];
				if (as->pid == cached_apid[cTag])
				{
					/* if we find the cached pids, this will be our default stream */

					audio_cached = i;
					break;
				}
			}
			/* also, we need to know the first non-mpeg (i.e. "ac3"/dts/...) stream */
			if (as->type != audioStream::atMPEG) {
				if (first_non_mpeg == -1)
					first_non_mpeg = i;
				else
				{
					for (int m = 0; m < eDVBService::nAudioCacheTags; m++)
					{
						if (as->pid == cached_apid[eDVBService::audioCacheTags[m]])
						{
							first_non_mpeg = i;
							break;
						}
					}
				}
			}
			if (!as->language_code.empty())
			{
				int x = 1;
				for (std::vector<std::string>::iterator
					it = autoaudio_languages.begin();
					x <= autoaudio_level && it != autoaudio_languages.end(); x++, it++)
				{
					bool languageFound = false;
					size_t pos = 0;
					char delimiter = '/';
					std::string audioStreamLanguages = program.audioStreams[i].language_code;
					audioStreamLanguages += delimiter;
					while ((pos = audioStreamLanguages.find(delimiter)) != std::string::npos)
					{
						if ((*it).find(as->language_code) != std::string::npos)
						{
						for (int m = 0; m < nAudioMapMain; m++)
						{
							eDVBService::cacheID cTag = audioMapMain[m].cacheTag;
							if (as->type == audioMapMain[m].streamType && (autoaudio_level > x || autoaudio[cTag] == -1))
							{
								autoaudio[cTag] = i;
								break;
							}
						}
						autoaudio_level = x;
						languageFound = true;
						break;
						}
						audioStreamLanguages.erase(0, pos + 1);
					}
					if (languageFound)
						break;
				}
			}
		}
		i = 0;
		for (std::vector<subtitleStream>::const_iterator
			ss(program.subtitleStreams.begin());
			ss != program.subtitleStreams.end(); ++ss, ++i)
		{
			if (!ss->language_code.empty())
			{
				int x = 1;
				for (std::vector<std::string>::iterator
					it2 = autosub_languages.begin();
					x <= autosub_level && it2 != autosub_languages.end(); x++, it2++)
				{
					if ((*it2).find(program.subtitleStreams[i].language_code) != std::string::npos)
					{
						autosub_level = x;
						if (ss->subtitling_type >= 0x10)
						{
							/* DVB subs */
							if (ss->subtitling_type >= 0x20)
								autosub_dvb_hearing = i;
							else
								autosub_dvb_normal = i;
						}
						else
						{
							/* TXT subs */
							if (ss->subtitling_type == 0x05)
								autosub_txt_hearing = i;
							else
								autosub_txt_normal = i;
						}
						break;
					}
				}
			}
		}

		bool defaultac3 = eSettings::audio_defaultac3;
		bool defaultddp = eSettings::audio_defaultddp;
		bool useaudio_cache = eSettings::audio_usecache;

		if (useaudio_cache && audio_cached != -1)
			program.defaultAudioStream = audio_cached;
		else if (defaultac3 && autoaudio[eDVBService::cAC3PID] != -1)
			program.defaultAudioStream = autoaudio[eDVBService::cAC3PID];
		else if (defaultddp && autoaudio[eDVBService::cDDPPID] != -1)
			program.defaultAudioStream = autoaudio[eDVBService::cDDPPID];
		else
		{
			int defaultAudio = -1;
			for (int m = 0; m < nAudioMapMain; m++)
			{
				eDVBService::cacheID cTag = audioMapMain[m].cacheTag;
				if (autoaudio[cTag] != -1)
				{
					defaultAudio = autoaudio[cTag];
					break;
				}
			}
			if (defaultAudio != -1)
				program.defaultAudioStream = defaultAudio;
			else if (first_non_mpeg != -1 && (defaultac3 || defaultddp))
				program.defaultAudioStream = first_non_mpeg;
		}

		bool allow_hearingimpaired = eSubtitleSettings::subtitle_hearingimpaired;
		bool default_hearingimpaired = eSubtitleSettings::subtitle_defaultimpaired;
		bool defaultdvb = eSubtitleSettings::subtitle_defaultdvb;
		int equallanguagemask = eSubtitleSettings::equal_languages;

		if (defaultdvb)
		{
			if (allow_hearingimpaired && default_hearingimpaired && autosub_dvb_hearing != -1)
				program.defaultSubtitleStream = autosub_dvb_hearing;
			else if (autosub_dvb_normal != -1)
				program.defaultSubtitleStream = autosub_dvb_normal;
			else if (allow_hearingimpaired && autosub_dvb_hearing != -1)
				program.defaultSubtitleStream = autosub_dvb_hearing;
			else if (allow_hearingimpaired && default_hearingimpaired && autosub_txt_hearing != -1)
				program.defaultSubtitleStream = autosub_txt_hearing;
			else if (autosub_txt_normal != -1)
				program.defaultSubtitleStream = autosub_txt_normal;
			else if (allow_hearingimpaired && autosub_txt_hearing != -1)
				program.defaultSubtitleStream = autosub_txt_hearing;
		}
		else
		{
			if (allow_hearingimpaired && default_hearingimpaired && autosub_txt_hearing != -1)
				program.defaultSubtitleStream = autosub_txt_hearing;
			else if (autosub_txt_normal != -1)
				program.defaultSubtitleStream = autosub_txt_normal;
			else if (allow_hearingimpaired && autosub_txt_hearing != -1)
				program.defaultSubtitleStream = autosub_txt_hearing;
			else if (allow_hearingimpaired && default_hearingimpaired && autosub_dvb_hearing != -1)
				program.defaultSubtitleStream = autosub_dvb_hearing;
			else if (autosub_dvb_normal != -1)
				program.defaultSubtitleStream = autosub_dvb_normal;
			else if (allow_hearingimpaired && autosub_dvb_hearing != -1)
				program.defaultSubtitleStream = autosub_dvb_hearing;
		}
		if (program.defaultSubtitleStream != -1 && (program.audioStreams[program.defaultAudioStream].language_code.empty() || ((equallanguagemask & (1<<(autosub_level-1))) == 0 && compareAudioSubtitleCode(program.subtitleStreams[program.defaultSubtitleStream].language_code, program.audioStreams[program.defaultAudioStream].language_code) == 0)))
			program.defaultSubtitleStream = -1;

		ret = 0;
	}
	else if ( m_service && !m_service->cacheEmpty() )
	{
		// Same entries, but different order from audioMapMain
		const static audioMap audioMapList[] = {
			{ audioStream::atAC3,   eDVBService::cAC3PID,    },
			{ audioStream::atAC4,   eDVBService::cAC4PID,    },
			{ audioStream::atDDP,   eDVBService::cDDPPID,    },
			{ audioStream::atAAC,   eDVBService::cAACAPID,   },
			{ audioStream::atDTS,   eDVBService::cDTSPID,    },
			{ audioStream::atLPCM,  eDVBService::cLPCMPID,   },
			{ audioStream::atDTSHD, eDVBService::cDTSHDPID,  },
			{ audioStream::atAACHE, eDVBService::cAACHEAPID, },
			{ audioStream::atDRA,   eDVBService::cDRAAPID,   },
			{ audioStream::atMPEG,  eDVBService::cMPEGAPID,  },
		};
		static const int nAudioMapList = sizeof audioMapList / sizeof audioMapList[0];
		int cached_pcrpid = m_service->getCacheEntry(eDVBService::cPCRPID),
			vpidtype = m_service->getCacheEntry(eDVBService::cVTYPE),
			pmtpid = m_service->getCacheEntry(eDVBService::cPMTPID),
			subpid = m_service->getCacheEntry(eDVBService::cSUBTITLE),
			cnt=0;
		if (pmtpid > 0)
		{
			program.pmtPid = pmtpid;
		}

		program.isCached = true;

		if ( vpidtype == -1 )
			vpidtype = videoStream::vtMPEG2;
		if ( cached_vpid != -1 )
		{
			videoStream s;
			s.pid = cached_vpid;
			s.type = vpidtype;
			program.videoStreams.push_back(s);
			++cnt;
		}
		for (int m = 0; m < nAudioMapList; m++)
		{
			eDVBService::cacheID cTag = audioMapList[m].cacheTag;
			if (cached_apid[cTag] != -1)
			{
				audioStream s;
				s.type = audioMapList[m].streamType;
				s.pid = cached_apid[cTag];
				s.rdsPid = -1;
				program.audioStreams.push_back(s);
				++cnt;
			}
		}
		if ( cached_pcrpid != -1 )
		{
			++cnt;
			program.pcrPid = cached_pcrpid;
		}
		if ( cached_tpid != -1 )
		{
			++cnt;
			program.textPid = cached_tpid;
		}
		if (subpid > 0)
		{
			subtitleStream s;
			s.pid = (subpid & 0xffff0000) >> 16;
			if (s.pid != program.textPid)
			{
				s.subtitling_type = 0x10;
				s.composition_page_id = (subpid >> 8) & 0xff;
				s.ancillary_page_id = subpid & 0xff;
				program.subtitleStreams.push_back(s);
				++cnt;
			}
		}
		if (cnt)
		{
			ret = 0;
		}
	}

	if (m_service && program.caids.empty()) // Add CAID from cache
	{
		CAID_LIST &caids = m_service->m_ca;
		for (CAID_LIST::iterator it(caids.begin()); it != caids.end(); ++it)
		{
			program::capid_pair pair;
			pair.caid = *it;
			pair.capid = -1; // not known yet
			pair.databytes.clear();
			program.caids.push_back(pair);
		}
	}

	if (m_demux)
	{
		m_demux->getCAAdapterID(adapter);
		program.adapterId = adapter;
		m_demux->getCADemuxID(demux);
		program.demuxId = demux;
	}

	m_cached_program = program;
	m_have_cached_program = true;
	return ret;
}

int eDVBServicePMTHandler::compareAudioSubtitleCode(const std::string &subtitleTrack, const std::string &audioTrack)
{
	std::size_t pos = audioTrack.find("/");
	if ( pos != std::string::npos)
	{
		std::string firstAudio = audioTrack.substr(0, pos);
		std::string secondAudio = audioTrack.substr(pos + 1);
		if (strcasecmp(subtitleTrack, firstAudio) == 0 || strcasecmp(subtitleTrack, secondAudio) == 0)
			return 0;
	}
	else
	{
		if (strcasecmp(subtitleTrack, audioTrack) == 0)
			return 0;
	}
	return -1;
}

int eDVBServicePMTHandler::getChannel(eUsePtr<iDVBChannel> &channel)
{
	if (!m_sr_channel && !m_reference.alternativeurl.empty())
	{

		ePtr<eDVBResourceManager> res_mgr;
		if ( !eDVBResourceManager::getInstance( res_mgr ) )
		{
			std::list<eDVBResourceManager::active_channel> list;
			res_mgr->getActiveChannels(list);
			if(list.size()) {

				eServiceReferenceDVB m_alternative_ref = eServiceReferenceDVB(m_reference.alternativeurl);
				char buf[30];
				sprintf(buf, "%x:%x:%x", m_alternative_ref.getTransportStreamID().get(), m_alternative_ref.getOriginalNetworkID().get(), m_alternative_ref.getDVBNamespace().get());
				std::string alternativeChannelID = std::string(buf);

				for (std::list<eDVBResourceManager::active_channel>::iterator i(list.begin()); i != list.end(); ++i)
				{
					std::string channelid = i->m_channel_id.toString();
					if (channelid == alternativeChannelID)
					{
						m_sr_channel = i->m_channel;
						res_mgr->feStateChanged();
						break;
					}
				}

			}
		}
	}

	channel = (m_sr_channel) ? m_sr_channel : m_channel;
	if (channel)
		return 0;
	else
		return -1;
}

int eDVBServicePMTHandler::getDataDemux(ePtr<iDVBDemux> &demux)
{
	demux = m_demux;
	if (demux)
		return 0;
	else
		return -1;
}

int eDVBServicePMTHandler::getDecodeDemux(ePtr<iDVBDemux> &demux)
{
	int ret=0;
		/* if we're using the decoding demux as data source
		   (for example in pvr playbacks), return that one. */
	if (m_pvr_channel)
	{
		demux = m_demux;
		return ret;
	}

	ASSERT(m_channel); /* calling without a previous ::tune is certainly bad. */

	ret = m_channel->getDemux(demux, iDVBChannel::capDecode);
	if (!ret)
		demux->getCADemuxID(m_decode_demux_num);

	return ret;
}

int eDVBServicePMTHandler::getPVRChannel(ePtr<iDVBPVRChannel> &pvr_channel)
{
	pvr_channel = m_pvr_channel;
	if (pvr_channel)
		return 0;
	else
		return -1;
}

void eDVBServicePMTHandler::SDTScanEvent(int event)
{
	switch (event)
	{
		case eDVBScan::evtFinish:
		{
			ePtr<iDVBChannelList> db;
			if (m_resourceManager->getChannelList(db) != 0)
				eDebug("[eDVBServicePMTHandler] no channel list");
			else
			{
				eDVBChannelID chid, curr_chid;
				m_reference.getChannelID(chid);
				curr_chid = m_dvb_scan->getCurrentChannelID();
				if (chid == curr_chid)
				{
					m_dvb_scan->insertInto(db, true);
					if(eDVBServicePMTHandler::m_debug)
						eDebug("[eDVBServicePMTHandler] sdt update done!");
				}
				else
				{
					eDebug("[eDVBServicePMTHandler] ignore sdt update data.... incorrect transponder tuned!!!");
					if (chid.dvbnamespace != curr_chid.dvbnamespace)
						eDebug("[eDVBServicePMTHandler] incorrect namespace. expected: %x current: %x",chid.dvbnamespace.get(), curr_chid.dvbnamespace.get());
					if (chid.transport_stream_id != curr_chid.transport_stream_id)
						eDebug("[eDVBServicePMTHandler] incorrect transport_stream_id. expected: %x current: %x",chid.transport_stream_id.get(), curr_chid.transport_stream_id.get());
					if (chid.original_network_id != curr_chid.original_network_id)
						eDebug("[eDVBServicePMTHandler] incorrect original_network_id. expected: %x current: %x",chid.original_network_id.get(), curr_chid.original_network_id.get());
				}
			}
			break;
		}

		default:
			break;
	}
}

int eDVBServicePMTHandler::tune(eServiceReferenceDVB &ref, int use_decode_demux, eCueSheet *cue, bool simulate, eDVBService *service, serviceType type, bool descramble)
{
	ePtr<iTsSource> s;
	return tuneExt(ref, s, NULL, cue, simulate, service, type, descramble);
}

int eDVBServicePMTHandler::tuneExt(eServiceReferenceDVB &ref, ePtr<iTsSource> &source, const char *streaminfo_file, eCueSheet *cue, bool simulate, eDVBService *service, serviceType type, bool descramble)
{
	RESULT res=0;
	m_reference = ref;
	m_reference.name = ""; // clear name, we don't need it

	/*
		* We need to m_use decode demux only when we are descrambling (demuxers > ca demuxers)
		* To avoid confusion with use_decode_demux now we look only descramble argument
	*/
	m_use_decode_demux = descramble;

	m_no_pat_entry_delay->stop();
	m_service_type = type;

		/* use given service as backup. This is used for time shift where we want to clone the live stream using the cache, but in fact have a PVR channel */
	m_service = service;

	/* is this a normal (non PVR) channel? */
	if (ref.path.empty())
	{
		eDVBChannelID chid;
		ref.getChannelID(chid);
		res = m_resourceManager->allocateChannel(chid, m_channel, simulate);
		if (!simulate)
		{
			if(eDVBServicePMTHandler::m_debug)
				eDebug("[eDVBServicePMTHandler] allocate Channel: res %d", res);
		}

		if (!res)
			serviceEvent(eventChannelAllocated);

		ePtr<iDVBChannelList> db;
		if (!m_resourceManager->getChannelList(db))
			db->getService((eServiceReferenceDVB&)m_reference, m_service);

		if (!res && !simulate && !m_ca_disabled)
			eDVBCIInterfaces::getInstance()->addPMTHandler(this);
	}
	else if (!simulate) // no simulation of playback services
	{
		if (!ref.getServiceID().get() /* incorrect sid in meta file or recordings.epl*/ )
		{
			eDVBTSTools tstools;
			bool b = source || !tstools.openFile(ref.path.c_str(), 1);
			eWarning("[eDVBServicePMTHandler] no .meta file found, trying to find PMT pid");
			if (source)
				tstools.setSource(source, NULL);
			if (b)
			{
				eDVBPMTParser::program program;
				if (!tstools.findPMT(program))
				{
					m_pmt_pid = program.pmtPid;
					if(eDVBServicePMTHandler::m_debug)
						eDebug("[eDVBServicePMTHandler] PMT pid %04x, service id %d", m_pmt_pid, program.serviceId);
					m_reference.setServiceID(program.serviceId);
				}
			}
			else
				eWarning("[eDVBServicePMTHandler] no valid source to find PMT pid!");
		}
		if(eDVBServicePMTHandler::m_debug)
			eDebug("[eDVBServicePMTHandler] alloc PVR");
			/* allocate PVR */
		eDVBChannelID chid;
		if (m_service_type == streamclient)
			ref.getChannelID(chid);
		res = m_resourceManager->allocatePVRChannel(chid, m_pvr_channel);
		if (res)
			eDebug("[eDVBServicePMTHandler] allocatePVRChannel failed!\n");
		m_channel = m_pvr_channel;
		if (!res && descramble)
			eDVBCIInterfaces::getInstance()->addPMTHandler(this);
	}

	if (!simulate)
	{
		if (m_channel)
		{
			m_channel->connectStateChange(
				sigc::mem_fun(*this, &eDVBServicePMTHandler::channelStateChanged),
				m_channelStateChanged_connection);
			m_last_channel_state = -1;
			channelStateChanged(m_channel);

			m_channel->connectEvent(
				sigc::mem_fun(*this, &eDVBServicePMTHandler::channelEvent),
				m_channelEvent_connection);

			if (ref.path.empty())
			{
				bool scandebug = eSimpleConfig::getBool("config.crash.debugDVBScan", false);
				m_dvb_scan = new eDVBScan(m_channel, true, scandebug);
				if (!eSimpleConfig::getBool("config.misc.disable_background_scan", false))
				{
					/*
					 * not starting a dvb scan triggers what appears to be a
					 * refcount bug (channel?/demux?), so we always start a scan,
					 * but ignore the results when background scanning is disabled
					 */
					m_dvb_scan->connectEvent(sigc::mem_fun(*this, &eDVBServicePMTHandler::SDTScanEvent), m_scan_event_connection);
				}
			}
		} else
		{
			if (res == eDVBResourceManager::errAllSourcesBusy)
				serviceEvent(eventNoResources);
			else /* errChidNotFound, errNoChannelList, errChannelNotInList, errNoSourceFound */
				serviceEvent(eventMisconfiguration);
			return res;
		}

		if (m_pvr_channel)
		{
			m_pvr_channel->setCueSheet(cue);

			if (m_pvr_channel->getDemux(m_pvr_demux_tmp, (!m_use_decode_demux) ? 0 : iDVBChannel::capDecode))
				eDebug("[eDVBServicePMTHandler] Allocating %s-decoding a demux for PVR channel failed.", m_use_decode_demux ? "" : "non-");
			else if (source)
				m_pvr_channel->playSource(source, streaminfo_file);
			else
				m_pvr_channel->playFile(ref.path.c_str());

			if (m_service_type == offline)
			{
				m_pvr_channel->setOfflineDecodeMode(eSimpleConfig::getInt("config.recording.offline_decode_delay", 1000));
			}
		}
	}

	return res;
}

void eDVBServicePMTHandler::free()
{
	m_dvb_scan = 0;

	if (m_ca_servicePtr)
	{
		int demuxes[2] = {0,0};
		uint8_t demuxid;
		uint8_t adapterid;
		m_demux->getCADemuxID(demuxid);
		m_demux->getCAAdapterID(adapterid);
		demuxes[0]=demuxid;
		if (m_decode_demux_num != 0xFF)
			demuxes[1]=m_decode_demux_num;
		else
			demuxes[1]=demuxes[0];
		ePtr<eTable<ProgramMapSection> > ptr;
		m_PMT.getCurrent(ptr);
		const eServiceReferenceDVB reference = eServiceReferenceDVB(m_reference.toReferenceString());
		eDVBCAHandler::getInstance()->unregisterService(reference, adapterid, demuxes, (int)m_service_type, ptr);
		m_ca_servicePtr = 0;
	}

	if (m_channel)
		eDVBCIInterfaces::getInstance()->removePMTHandler(this);

	if (m_pvr_channel)
	{
		m_pvr_channel->stop();
		m_pvr_channel->setCueSheet(0);
	}

	m_OC.stop();
	m_AIT.stop();
	m_PMT.stop();
	m_PAT.stop();
	m_service = 0;
	m_channel = 0;
	m_sr_channel = 0;
	m_pvr_channel = 0;
	m_demux = 0;
}

void eDVBServicePMTHandler::addCaHandler()
{
	m_ca_disabled = false;
	if (m_channel)
	{
		eDVBCIInterfaces::getInstance()->addPMTHandler(this);
		if (m_pmt_ready)
		{
			eDVBCIInterfaces::getInstance()->recheckPMTHandlers();
			eDVBCIInterfaces::getInstance()->gotPMT(this);
		}
	}
}

void eDVBServicePMTHandler::removeCaHandler()
{
	m_ca_disabled = true;
	if (m_channel)
		eDVBCIInterfaces::getInstance()->removePMTHandler(this);
}

bool eDVBServicePMTHandler::isCiConnected()
{
	return eDVBCIInterfaces::getInstance()->isCiConnected(this);
}
