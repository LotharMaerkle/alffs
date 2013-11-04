package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.util.HashMap;
import java.util.Map;

import org.alfresco.service.cmr.repository.NodeRef;
import org.apache.commons.lang.StringUtils;
import org.json.JSONException;
import org.json.simple.JSONValue;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public class OpenResource extends AbstractIOResource {
	@SuppressWarnings("unchecked")
	@Override
	public void doPost(WebScriptRequest req, WebScriptResponse res)
			throws IOException, JSONException {
		res.setContentType(JSON_MIMETYPE);
		res.setContentEncoding(DEFAULT_ENCODING);
		res.setStatus(Status.STATUS_OK);

		Map<String, Object> jsonParam = (Map<String, Object>) JSONValue
				.parse(req.getContent().getContent());

		String base = (String) jsonParam.get(PARAM_BASE);
		String path = (String) jsonParam.get(PARAM_PATH);
		//Long flags = (Long) jsonParam.get(PARAM_FLAGS);

		if (StringUtils.isBlank(base) || StringUtils.isBlank(path)) {
			res.setStatus(Status.STATUS_BAD_REQUEST);
			Map<String, Object> jsonResponse = new HashMap<String, Object>();
			jsonResponse.put("message", "garbage in garbage out");
			JSONValue.writeJSONString(jsonResponse, res.getWriter());
			return;
		}

		NodeRef nodeRef = locateNode(base, path);
		if (nodeRef == null) {
			sendError(res, "no such file or directory", ERROR_NOENT,
					Status.STATUS_NOT_FOUND);
			return;
		}

		Map<String, Object> jsonResponse = new HashMap<String, Object>();
		jsonResponse.put("nodeRef", nodeRef.toString());
		jsonResponse.put("uuid", nodeRef.getId());
		JSONValue.writeJSONString(jsonResponse, res.getWriter());
	}
}
