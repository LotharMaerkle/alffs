package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.util.Date;
import java.util.HashMap;
import java.util.Map;

import org.alfresco.model.ContentModel;
import org.alfresco.repo.policy.BehaviourFilter;
import org.alfresco.service.cmr.repository.NodeRef;
import org.alfresco.util.ISO8601DateFormat;
import org.apache.commons.lang.StringUtils;
import org.json.JSONException;
import org.json.simple.JSONValue;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public class UTimeNSResource extends AbstractIOResource {
	private BehaviourFilter behaviourFilter;

	@Override
	public void doPost(WebScriptRequest req, WebScriptResponse res)
			throws IOException, JSONException {
		res.setContentType(JSON_MIMETYPE);
		res.setContentEncoding(DEFAULT_ENCODING);
		res.setStatus(Status.STATUS_OK);

		@SuppressWarnings("unchecked")
		Map<String, Object> jsonParam = (Map<String, Object>) JSONValue
				.parse(req.getContent().getContent());

		String base = req.getParameter(PARAM_BASE);
		String path = req.getParameter(PARAM_PATH);
		String atime_sec_str = (String) jsonParam.get(PARAM_ATIME_SEC);
		String atime_nsec_str = (String) jsonParam.get(PARAM_ATIME_NSEC);
		String mtime_sec_str = (String) jsonParam.get(PARAM_MTIME_SEC);
		String mtime_nsec_str = (String) jsonParam.get(PARAM_MTIME_NSEC);

		if (StringUtils.isBlank(base) || StringUtils.isBlank(path)) {
			res.setStatus(Status.STATUS_BAD_REQUEST);
			sendError(res, "garbage in garbage out", ERROR_IO);
			return;
		}

		NodeRef nodeRef = locateNode(base, path);
		if (nodeRef == null) {
			sendError(res, "no such file or directory", ERROR_NOENT,
					Status.STATUS_NOT_FOUND);
			return;
		}

		long mtime = 0;
		if (StringUtils.isNotBlank(mtime_sec_str)) {
			mtime = Long.parseLong(mtime_sec_str) * 1000;
		}
		if (StringUtils.isNotBlank(mtime_nsec_str)) {
			mtime += Long.parseLong(mtime_nsec_str);
		}

		Map<String, Object> jsonResponse = new HashMap<String, Object>();

		if (mtime > 0) {
			Date modDate = new Date(mtime);
			try {
				behaviourFilter.disableBehaviour(nodeRef,
						ContentModel.ASPECT_AUDITABLE);
					serviceRegistry.getNodeService().setProperty(nodeRef,
						ContentModel.PROP_MODIFIED, modDate);
				jsonResponse.put("cm:modified",
						ISO8601DateFormat.format(modDate));
			} finally {
				behaviourFilter.enableBehaviour(nodeRef,
						ContentModel.ASPECT_AUDITABLE);
			}

		}
		JSONValue.writeJSONString(jsonResponse, res.getWriter());
	}

	public BehaviourFilter getBehaviourFilter() {
		return behaviourFilter;
	}

	public void setBehaviourFilter(BehaviourFilter behaviourFilter) {
		this.behaviourFilter = behaviourFilter;
	}

}
