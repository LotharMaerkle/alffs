package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import org.alfresco.service.cmr.model.FileInfo;
import org.alfresco.service.cmr.repository.NodeRef;
import org.apache.commons.lang.StringUtils;
import org.json.JSONException;
import org.json.simple.JSONValue;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public class ReadDirResource extends AbstractIOResource {
	@Override
	public void doGet(WebScriptRequest req, WebScriptResponse res)
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

		Map<String, Object> jsonResponse = new HashMap<String, Object>();
		
		
		
		List<FileInfo> list = serviceRegistry.getFileFolderService().list(nodeRef);
		List<Map<String, Object>> dirents = new ArrayList<Map<String, Object>>(list.size());
		for(FileInfo finfo: list) {
			Map<String, Object> ent = new HashMap<String, Object>();
			ent.put("name", finfo.getName());
			if (finfo.isLink()) {
				ent.put("type", S_IFLNK);
			} else if (finfo.isFolder()) {
				ent.put("type", S_IFDIR);
			} else {
				ent.put("type", S_IFREG);
			}
			
			dirents.add(ent);
		}
		
		jsonResponse.put("total", dirents.size());
		jsonResponse.put("dirents", dirents);
		
		JSONValue.writeJSONString(jsonResponse, res.getWriter());
	}
}
