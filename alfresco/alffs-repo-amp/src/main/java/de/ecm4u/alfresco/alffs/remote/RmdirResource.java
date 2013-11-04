package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.util.HashMap;
import java.util.Map;

import org.alfresco.model.ContentModel;
import org.alfresco.service.cmr.repository.NodeRef;
import org.alfresco.service.namespace.QName;
import org.apache.commons.lang.StringUtils;
import org.json.JSONException;
import org.json.simple.JSONValue;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public class RmdirResource extends AbstractIOResource {
	@Override
	public void doDelete(WebScriptRequest req, WebScriptResponse res)
			throws IOException, JSONException {
		res.setContentType(JSON_MIMETYPE);
		res.setContentEncoding(DEFAULT_ENCODING);
		res.setStatus(Status.STATUS_OK);

		String base = req.getParameter(PARAM_BASE);
		String path = req.getParameter(PARAM_PATH);

		if (StringUtils.isBlank(base) || StringUtils.isBlank(path)) {
			res.setStatus(Status.STATUS_BAD_REQUEST);
			Map<String, Object> jsonResponse = new HashMap<String, Object>();
			jsonResponse.put("message", "garbage in garbage out");
			JSONValue.writeJSONString(jsonResponse, res.getWriter());
			return;
		}

		NodeRef nodeRef = locateNode(base, path);
		if (nodeRef == null) {
			res.setStatus(Status.STATUS_NOT_FOUND);
			Map<String, Object> jsonResponse = new HashMap<String, Object>();
			jsonResponse.put("message", "no such file or directory");
			JSONValue.writeJSONString(jsonResponse, res.getWriter());
			return;
		}

		QName type = serviceRegistry.getNodeService().getType(nodeRef);
		if (serviceRegistry.getDictionaryService().isSubClass(type,
				ContentModel.TYPE_FOLDER)) {
			// check if empty
			int childs = serviceRegistry.getNodeService().countChildAssocs(nodeRef, true);
			if(childs == 0) {
				serviceRegistry.getNodeService().deleteNode(nodeRef);
			} else {
				res.setStatus(Status.STATUS_BAD_REQUEST);
				Map<String, Object> jsonResponse = new HashMap<String, Object>();
				jsonResponse.put("message", "directory not empty");
				jsonResponse.put("errno", "ENOTEMPTY");
				JSONValue.writeJSONString(jsonResponse, res.getWriter());				
				return;
			}
		} else {
			res.setStatus(Status.STATUS_BAD_REQUEST);
			Map<String, Object> jsonResponse = new HashMap<String, Object>();
			jsonResponse.put("message", "not a directory");
			jsonResponse.put("errno", "ENOTDIR");
			JSONValue.writeJSONString(jsonResponse, res.getWriter());
			return;
		}

		Map<String, Object> jsonResponse = new HashMap<String, Object>();

		JSONValue.writeJSONString(jsonResponse, res.getWriter());

	}

}
