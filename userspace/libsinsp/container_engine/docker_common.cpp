/*
Copyright (C) 2013-2019 Draios Inc dba Sysdig.

This file is part of sysdig.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

*/

#include "container_engine/docker.h"
#include "cgroup_list_counter.h"
#include "sinsp.h"
#include "sinsp_int.h"
#include "container.h"
#include "utils.h"
#include <unordered_set>

using namespace libsinsp::container_engine;

docker_async_source::docker_async_source(uint64_t max_wait_ms, uint64_t ttl_ms, sinsp *inspector)
	: async_key_value_source(max_wait_ms, ttl_ms),
	  m_inspector(inspector),
	  m_docker_unix_socket_path("/var/run/docker.sock"),
#ifdef _WIN32
	  m_api_version("/v1.30"),
#else
	  m_api_version("/v1.24"),
	  m_curlm(NULL),
	  m_curl(NULL)
#endif
{
	init_docker_conn();
}

docker_async_source::~docker_async_source()
{
	this->stop();
	g_logger.format(sinsp_logger::SEV_DEBUG,
			"docker_async: Source destructor");

	free_docker_conn();
}

void docker_async_source::run_impl()
{
	std::string container_id;

	while (dequeue_next_key(container_id))
	{
		g_logger.format(sinsp_logger::SEV_DEBUG,
				"docker_async (%s): Source dequeued key",
				container_id.c_str());

		container_lookup_result res;

		res.m_successful = true;
		res.m_container_info.m_type = CT_DOCKER;
		res.m_container_info.m_id = container_id;

		if(!parse_docker(container_id, &res.m_container_info))
		{
			// This is not always an error e.g. when using
			// containerd as the runtime. Since the cgroup
			// names are often identical between
			// containerd and docker, we have to try to
			// fetch both.
			g_logger.format(sinsp_logger::SEV_DEBUG,
					"docker_async (%s): Failed to get Docker metadata, returning successful=false",
					container_id.c_str());
			res.m_successful = false;
		}

		g_logger.format(sinsp_logger::SEV_DEBUG,
				"docker_async (%s): Parse successful, storing value",
				container_id.c_str());

		// Return a result object either way, to ensure any
		// new container callbacks are called.
		store_value(container_id, res);
	}
}

bool docker_async_source::m_query_image_info = true;

void docker::parse_json_mounts(const Json::Value &mnt_obj, vector<sinsp_container_info::container_mount_info> &mounts)
{
	if(!mnt_obj.isNull() && mnt_obj.isArray())
	{
		for(uint32_t i=0; i<mnt_obj.size(); i++)
		{
			const Json::Value &mount = mnt_obj[i];
			mounts.emplace_back(mount["Source"], mount["Destination"],
					    mount["Mode"], mount["RW"],
					    mount["Propagation"]);
		}
	}
}

bool docker_async_source::get_k8s_pod_spec(const Json::Value &config_obj,
					   Json::Value &spec)
{
	std::string cfg_str;
	Json::Reader reader;
	std::string k8s_label = "annotation.kubectl.kubernetes.io/last-applied-configuration";

	if(config_obj.isNull() ||
	   !config_obj.isMember("Labels") ||
	   !config_obj["Labels"].isMember(k8s_label))
	{
		return false;
	}

	// The pod spec is stored as a stringified json label on the container
	cfg_str = config_obj["Labels"][k8s_label].asString();

	if(cfg_str == "")
	{
		return false;
	}

	Json::Value cfg;
	if(!reader.parse(cfg_str.c_str(), cfg))
	{
		g_logger.format(sinsp_logger::SEV_WARNING, "Could not parse pod config '%s'", cfg_str.c_str());
		return false;
	}

	if(!cfg.isMember("spec") ||
	   !cfg["spec"].isMember("containers") ||
	   !cfg["spec"]["containers"].isArray())
	{
		return false;
	}

	// XXX/mstemm how will this work with init containers?
	spec = cfg["spec"]["containers"][0];

	return true;
}

std::string docker_async_source::normalize_arg(const std::string &arg)
{
	std::string ret = arg;

	if(ret.empty())
	{
		return ret;
	}

	// Remove pairs of leading/trailing " or ' chars, if present
	while(ret.front() == '"' || ret.front() == '\'')
	{
		if(ret.back() == ret.front())
		{
			ret.pop_back();
			ret.erase(0, 1);
		}
	}

	return ret;
}

void docker_async_source::parse_healthcheck(const Json::Value &healthcheck_obj,
					    sinsp_container_info *container)
{
	g_logger.format(sinsp_logger::SEV_DEBUG,
			"docker (%s): Trying to parse healthcheck from %s",
			container->m_id.c_str(), Json::FastWriter().write(healthcheck_obj).c_str());

	if(healthcheck_obj.isNull())
	{
		g_logger.format(sinsp_logger::SEV_WARNING, "Could not parse health check from %s (No Healthcheck property)",
				Json::FastWriter().write(healthcheck_obj).c_str());

		return;
	}

	if(!healthcheck_obj.isMember("Test"))
	{
		g_logger.format(sinsp_logger::SEV_WARNING, "Could not parse health check from %s (Healthcheck does not have Test property)",
				Json::FastWriter().write(healthcheck_obj).c_str());

		return;
	}

	const Json::Value &test_obj = healthcheck_obj["Test"];

	if(!test_obj.isArray())
	{
		g_logger.format(sinsp_logger::SEV_WARNING, "Could not parse health check from %s (Healthcheck Test property is not array)",
				Json::FastWriter().write(healthcheck_obj).c_str());
		return;
	}

	if(test_obj.size() == 1)
	{
		if(test_obj[0].asString() != "NONE")
		{
			g_logger.format(sinsp_logger::SEV_WARNING, "Could not parse health check from %s (Expected NONE for single-element Test array)",
					Json::FastWriter().write(healthcheck_obj).c_str());
		}
		return;
	}

	if(test_obj[0].asString() == "CMD")
	{
		std::string exe = normalize_arg(test_obj[1].asString());
		std::vector<std::string> args;

		for(uint32_t i = 2; i < test_obj.size(); i++)
		{
			args.push_back(normalize_arg(test_obj[i].asString()));
		}

		g_logger.format(sinsp_logger::SEV_DEBUG,
				"docker (%s): Setting PT_HEALTHCHECK exe=%s nargs=%d",
				container->m_id.c_str(), exe.c_str(), args.size());

		container->m_health_probes.emplace_back(sinsp_container_info::container_health_probe::PT_HEALTHCHECK,
							std::move(exe),
							std::move(args));
	}
	else if(test_obj[0].asString() == "CMD-SHELL")
	{
		std::string exe = "/bin/sh";
		std::vector<std::string> args;

		args.push_back("-c");
		args.push_back(test_obj[1].asString());

		g_logger.format(sinsp_logger::SEV_DEBUG,
				"docker (%s): Setting PT_HEALTHCHECK exe=%s nargs=%d",
				container->m_id.c_str(), exe.c_str(), args.size());

		container->m_health_probes.emplace_back(sinsp_container_info::container_health_probe::PT_HEALTHCHECK,
							std::move(exe),
							std::move(args));
	}
	else
	{
		g_logger.format(sinsp_logger::SEV_WARNING, "Could not parse health check from %s (Expected CMD/CMD-SHELL for multi-element Test array)",
				Json::FastWriter().write(healthcheck_obj).c_str());
		return;
	}
}

bool docker_async_source::parse_liveness_readiness_probe(const Json::Value &probe_obj,
							 sinsp_container_info::container_health_probe::probe_type ptype,
							 sinsp_container_info *container)
{
	if(probe_obj.isNull() ||
	   !probe_obj.isMember("exec") ||
	   !probe_obj["exec"].isMember("command"))
	{
		g_logger.format(sinsp_logger::SEV_WARNING, "Could not parse liveness/readiness probe from %s",
				Json::FastWriter().write(probe_obj).c_str());
		return false;
	}

	const Json::Value command_obj = probe_obj["exec"]["command"];

	if(!command_obj.isNull() && command_obj.isArray())
	{
		std::string exe;
		std::vector<std::string> args;

		exe = normalize_arg(command_obj[0].asString());
		for(uint32_t i = 1; i < command_obj.size(); i++)
		{
			args.push_back(normalize_arg(command_obj[i].asString()));
		}

		g_logger.format(sinsp_logger::SEV_DEBUG,
				"docker (%s): Setting %s exe=%s nargs=%d",
				container->m_id.c_str(),
				sinsp_container_info::container_health_probe::probe_type_names[ptype].c_str(),
				exe.c_str(), args.size());

		container->m_health_probes.emplace_back(ptype, std::move(exe), std::move(args));
	}

	return true;
}

void docker_async_source::parse_health_probes(const Json::Value &config_obj,
					      sinsp_container_info *container)
{
	Json::Value spec;
	bool liveness_readiness_added = false;

	// When parsing the full container json for live containers, a label contains stringified json that
	// contains the probes.
	if (get_k8s_pod_spec(config_obj, spec))
	{
		if(spec.isMember("livenessProbe"))
		{
			if(parse_liveness_readiness_probe(spec["livenessProbe"],
							  sinsp_container_info::container_health_probe::PT_LIVENESS_PROBE,
							  container))
			{
				liveness_readiness_added = true;
			}
		}
		else if(spec.isMember("readinessProbe"))
		{
			if(parse_liveness_readiness_probe(spec["readinessProbe"],
							  sinsp_container_info::container_health_probe::PT_READINESS_PROBE,
							  container))
			{
				liveness_readiness_added = true;
			}
		}
	}

	// To avoid any confusion about containers that both refer to
	// a healthcheck and liveness/readiness probe, we only
	// consider a healthcheck if no liveness/readiness was added.
	if(!liveness_readiness_added && config_obj.isMember("Healthcheck"))
	{
		parse_healthcheck(config_obj["Healthcheck"], container);
	}
}

void docker_async_source::set_query_image_info(bool query_image_info)
{
	g_logger.format(sinsp_logger::SEV_DEBUG,
			"docker_async: Setting query_image_info=%s",
			(query_image_info ? "true" : "false"));

	m_query_image_info = query_image_info;
}

std::string docker::s_incomplete_info_name = "incomplete";

bool docker::resolve(sinsp_container_manager* manager, sinsp_threadinfo* tinfo, bool query_os_for_missing_info)
{
	std::string container_id, container_name;
	sinsp_container_info *existing_container_info;

	if(!detect_docker(tinfo, container_id, container_name))
	{
		return false;
	}

	if(!m_docker_info_source)
	{
		g_logger.log("docker_async: Creating docker async source",
			     sinsp_logger::SEV_DEBUG);
		uint64_t max_wait_ms = 10000;
		docker_async_source *src = new docker_async_source(docker_async_source::NO_WAIT_LOOKUP, max_wait_ms, manager->get_inspector());
		m_docker_info_source.reset(src);
	}

	tinfo->m_container_id = container_id;

	existing_container_info = manager->get_container(container_id);

	if(!existing_container_info)
	{
		// Add a minimal container_info object where only the
		// container id, (possibly) name, and a container
		// image = incomplete is filled in. This may be
		// overidden later once parse_docker_async completes.
		sinsp_container_info container_info;

		g_logger.format(sinsp_logger::SEV_DEBUG,
				"docker_async (%s): No existing container info, creating initial stub info",
				container_id.c_str());

		container_info.m_type = CT_DOCKER;
		container_info.m_id = container_id;
		container_info.m_name = container_name;
		container_info.m_image = s_incomplete_info_name;
		container_info.m_imageid = s_incomplete_info_name;
		container_info.m_imagerepo = s_incomplete_info_name;
		container_info.m_imagetag = s_incomplete_info_name;
		container_info.m_imagedigest = s_incomplete_info_name;
		container_info.m_metadata_complete = false;

		manager->add_container(container_info, tinfo);

		existing_container_info = manager->get_container(container_id);
	}

#ifdef HAS_CAPTURE
	// Possibly start a lookup for this container info
	if(!existing_container_info->m_metadata_complete &&
	    query_os_for_missing_info)
	{
		// give docker a chance to return metadata for this container
		parse_docker_async(manager->get_inspector(), container_id, manager);
	}
#endif

	// Returning true will prevent other container engines from
	// trying to resolve the container, so only return true if we
	// have complete metadata.
	return existing_container_info->m_metadata_complete;
}

void docker::parse_docker_async(sinsp *inspector, std::string &container_id, sinsp_container_manager *manager)
{
	auto cb = [manager](const std::string &container_id, const container_lookup_result &res)
        {
		g_logger.format(sinsp_logger::SEV_DEBUG,
				"docker_async (%s): Source callback result successful=%s",
				container_id.c_str(),
				(res.m_successful ? "true" : "false"));

		if(res.m_successful)
		{
			manager->notify_new_container(res.m_container_info);
		}
	};

        container_lookup_result result;

	if (m_docker_info_source->lookup(container_id, result, cb))
	{
		// if a previous lookup call already found the metadata, process it now
		cb(container_id, result);

		// This should *never* happen, as ttl is 0 (never wait)
		g_logger.format(sinsp_logger::SEV_ERROR,
				"docker_async (%s): Unexpected immediate return from docker_info_source.lookup()",
				container_id.c_str());
	}
}

bool docker_async_source::parse_docker(std::string &container_id, sinsp_container_info *container)
{
	string json;

	g_logger.format(sinsp_logger::SEV_DEBUG,
			"docker_async (%s): Looking up info for container",
			container_id.c_str());

	docker_response resp = get_docker(build_request("/containers/" + container_id + "/json"), json);
	switch(resp) {
		case docker_response::RESP_BAD_REQUEST:
			g_logger.format(sinsp_logger::SEV_DEBUG,
					"docker_async (%s): Initial url fetch failed, trying w/o api version",
					container_id.c_str());

			m_api_version = "";
			json = "";
			resp = get_docker(build_request("/containers/" + container_id + "/json"), json);
			if (resp == docker_response::RESP_OK)
			{
				break;
			}
			/* FALLTHRU */
		case docker_response::RESP_ERROR:
			g_logger.format(sinsp_logger::SEV_DEBUG,
					"docker_async (%s): Url fetch failed, returning false",
					container_id.c_str());

			return false;

		case docker_response::RESP_OK:
			break;
	}

	g_logger.format(sinsp_logger::SEV_DEBUG,
			"docker_async (%s): Parsing containers response \"%s\"",
			container_id.c_str(),
			json.c_str());

	Json::Value root;
	Json::Reader reader;
	bool parsingSuccessful = reader.parse(json, root);
	if(!parsingSuccessful)
	{
		g_logger.format(sinsp_logger::SEV_ERROR,
				"docker_async (%s): Could not parse json \"%s\", returning false",
				container_id.c_str(),
				json.c_str());

		ASSERT(false);
		return false;
	}

	const Json::Value& config_obj = root["Config"];

	container->m_image = config_obj["Image"].asString();

	string imgstr = root["Image"].asString();
	size_t cpos = imgstr.find(":");
	if(cpos != string::npos)
	{
		container->m_imageid = imgstr.substr(cpos + 1);
	}

	parse_health_probes(config_obj, container);

	// containers can be spawned using just the imageID as image name,
	// with or without the hash prefix (e.g. sha256:)
	bool no_name = !container->m_imageid.empty() &&
		strncmp(container->m_image.c_str(), container->m_imageid.c_str(),
			MIN(container->m_image.length(), container->m_imageid.length())) == 0;
	no_name |= !imgstr.empty() &&
		strncmp(container->m_image.c_str(), imgstr.c_str(),
			MIN(container->m_image.length(), imgstr.length())) == 0;

	if(!no_name || !m_query_image_info)
	{
		string hostname, port;
		sinsp_utils::split_container_image(container->m_image,
						   hostname,
						   port,
						   container->m_imagerepo,
						   container->m_imagetag,
						   container->m_imagedigest,
						   false);
	}

	if(m_query_image_info && !container->m_imageid.empty() &&
	   (no_name || container->m_imagedigest.empty() || (!container->m_imagedigest.empty() && container->m_imagetag.empty())))
	{
		g_logger.format(sinsp_logger::SEV_DEBUG,
				"docker_async (%s) image (%s): Fetching image info",
				container_id.c_str(),
				container->m_imageid.c_str());

		string img_json;
		if(get_docker(build_request("/images/" + container->m_imageid + "/json?digests=1"), img_json) == docker_response::RESP_OK)
		{
			g_logger.format(sinsp_logger::SEV_DEBUG,
					"docker_async (%s) image (%s): Image info fetch returned \"%s\"",
					container_id.c_str(),
					container->m_imageid.c_str(),
					img_json.c_str());

			Json::Value img_root;
			if(reader.parse(img_json, img_root))
			{
				// img_root["RepoDigests"] contains only digests for images pulled from registries.
				// If an image gets retagged and is never pushed to any registry, we will not find
				// that entry in container->m_imagerepo. Also, for locally built images we have the
				// same issue. This leads to container->m_imagedigest being empty as well.
				unordered_set<std::string> imageDigestSet;
				for(const auto& rdig : img_root["RepoDigests"])
				{
					if(rdig.isString())
					{
						string repodigest = rdig.asString();
						string digest = repodigest.substr(repodigest.find('@')+1);
						imageDigestSet.insert(digest);
						if(container->m_imagerepo.empty())
						{
							container->m_imagerepo = repodigest.substr(0, repodigest.find('@'));
						}
						if(repodigest.find(container->m_imagerepo) != string::npos)
						{
							container->m_imagedigest = digest;
							break;
						}
					}
				}
				for(const auto& rtag : img_root["RepoTags"])
				{
					if(rtag.isString())
					{
						string repotag = rtag.asString();
						if(container->m_imagerepo.empty())
						{
							container->m_imagerepo = repotag.substr(0, repotag.rfind(":"));
						}
						if(repotag.find(container->m_imagerepo) != string::npos)
						{
							container->m_imagetag = repotag.substr(repotag.rfind(":")+1);
							break;
						}
					}
				}
				// fix image digest for locally tagged images or multiple repo digests.
				// Case 1: One repo digest with many tags.
				// Case 2: Many repo digests with the same digest value.
				if(container->m_imagedigest.empty() && imageDigestSet.size() == 1) {
					container->m_imagedigest = *imageDigestSet.begin();
				}
			}
			else
			{
				g_logger.format(sinsp_logger::SEV_ERROR,
						"docker_async (%s) image (%s): Could not parse json image info \"%s\"",
						container_id.c_str(),
						container->m_imageid.c_str(),
						img_json.c_str());
			}
		}
		else
		{
			g_logger.format(sinsp_logger::SEV_ERROR,
					"docker_async (%s) image (%s): Could not fetch image info",
					container_id.c_str(),
					container->m_imageid.c_str());
		}

	}
	if(container->m_imagetag.empty())
	{
		container->m_imagetag = "latest";
	}

	container->m_name = root["Name"].asString();
	// k8s Docker container names could have '/' as the first character.
	if(!container->m_name.empty() && container->m_name[0] == '/')
	{
		container->m_name = container->m_name.substr(1);
	}
	if(container->m_name.find("k8s_POD") == 0)
	{
		container->m_is_pod_sandbox = true;
	}

	const Json::Value& net_obj = root["NetworkSettings"];

	string ip = net_obj["IPAddress"].asString();

	if(ip.empty())
	{
 		const Json::Value& hconfig_obj = root["HostConfig"];
		string net_mode = hconfig_obj["NetworkMode"].asString();

		if(strncmp(net_mode.c_str(), "container:", strlen("container:")) == 0)
		{
			std::string secondary_container_id = net_mode.substr(net_mode.find(":") + 1);

			sinsp_container_info pcnt;
			pcnt.m_id = secondary_container_id;

			// This is a *blocking* fetch of the
			// secondary container, but we're in a
			// separate thread so this is ok.
			g_logger.format(sinsp_logger::SEV_DEBUG,
					"docker_async (%s), secondary (%s): Doing blocking fetch of secondary container",
					container_id.c_str(),
					secondary_container_id.c_str());

			if (parse_docker(secondary_container_id, &pcnt))
			{
				g_logger.format(sinsp_logger::SEV_DEBUG,
						"docker_async (%s), secondary (%s): Secondary fetch successful",
						container_id.c_str(),
						secondary_container_id.c_str());
				container->m_container_ip = pcnt.m_container_ip;
			}
			else
			{
				g_logger.format(sinsp_logger::SEV_ERROR,
						"docker_async (%s), secondary (%s): Secondary fetch failed",
						container_id.c_str(),
						secondary_container_id.c_str());
			}
		}
	}
	else
	{
		if(inet_pton(AF_INET, ip.c_str(), &container->m_container_ip) == -1)
		{
			ASSERT(false);
		}
		container->m_container_ip = ntohl(container->m_container_ip);
	}

	vector<string> ports = net_obj["Ports"].getMemberNames();
	for(vector<string>::const_iterator it = ports.begin(); it != ports.end(); ++it)
	{
		size_t tcp_pos = it->find("/tcp");
		if(tcp_pos == string::npos)
		{
			continue;
		}

		uint16_t container_port = atoi(it->c_str());

		const Json::Value& v = net_obj["Ports"][*it];
		if(v.isArray())
		{
			for(uint32_t j = 0; j < v.size(); ++j)
			{
				sinsp_container_info::container_port_mapping port_mapping;

				ip = v[j]["HostIp"].asString();
				string port = v[j]["HostPort"].asString();

				if(inet_pton(AF_INET, ip.c_str(), &port_mapping.m_host_ip) == -1)
				{
					ASSERT(false);
					continue;
				}
				port_mapping.m_host_ip = ntohl(port_mapping.m_host_ip);

				port_mapping.m_container_port = container_port;
				port_mapping.m_host_port = atoi(port.c_str());
				container->m_port_mappings.push_back(port_mapping);
			}
		}
	}

	vector<string> labels = config_obj["Labels"].getMemberNames();
	for(vector<string>::const_iterator it = labels.begin(); it != labels.end(); ++it)
	{
		string val = config_obj["Labels"][*it].asString();
		container->m_labels[*it] = val;
	}

	const Json::Value& env_vars = config_obj["Env"];

	for(const auto& env_var : env_vars)
	{
		if(env_var.isString())
		{
			container->m_env.emplace_back(env_var.asString());
		}
	}

	const auto& host_config_obj = root["HostConfig"];
	container->m_memory_limit = host_config_obj["Memory"].asInt64();
	container->m_swap_limit = host_config_obj["MemorySwap"].asInt64();
	const auto cpu_shares = host_config_obj["CpuShares"].asInt64();
	if(cpu_shares > 0)
	{
		container->m_cpu_shares = cpu_shares;
	}
	container->m_cpu_quota = host_config_obj["CpuQuota"].asInt64();
	const auto cpu_period = host_config_obj["CpuPeriod"].asInt64();
	if(cpu_period > 0)
	{
		container->m_cpu_period = cpu_period;
	}
	const auto cpuset_cpus = host_config_obj["CpusetCpus"].asString();
	if (!cpuset_cpus.empty())
	{
		libsinsp::cgroup_list_counter counter;
		container->m_cpuset_cpu_count = counter(cpuset_cpus.c_str(), sinsp_logger::SEV_DEBUG);
	}
	const Json::Value& privileged = host_config_obj["Privileged"];
	if(!privileged.isNull() && privileged.isBool())
	{
		container->m_privileged = privileged.asBool();
	}

	docker::parse_json_mounts(root["Mounts"], container->m_mounts);

#ifdef HAS_ANALYZER
	sinsp_utils::find_env(container->m_sysdig_agent_conf, container->get_env(), "SYSDIG_AGENT_CONF");
	// container->m_sysdig_agent_conf = get_docker_env(env_vars, "SYSDIG_AGENT_CONF");
#endif

	g_logger.format(sinsp_logger::SEV_DEBUG,
			"docker_async (%s): parse_docker returning true",
			container_id.c_str());
	return true;
}

