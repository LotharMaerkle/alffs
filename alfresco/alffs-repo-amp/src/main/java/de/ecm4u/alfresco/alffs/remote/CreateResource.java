package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.util.HashMap;
import java.util.Map;

import org.alfresco.model.ContentModel;
import org.alfresco.service.cmr.model.FileExistsException;
import org.alfresco.service.cmr.model.FileInfo;
import org.alfresco.service.cmr.repository.ContentWriter;
import org.alfresco.service.cmr.repository.NodeRef;
import org.alfresco.service.namespace.QName;
import org.apache.commons.io.FilenameUtils;
import org.apache.commons.lang.StringUtils;
import org.json.JSONException;
import org.json.simple.JSONValue;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public class CreateResource extends AbstractIOResource {
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
		String type = (String) jsonParam.get(PARAM_TYPE);
		Long flags = (Long)jsonParam.get(PARAM_FLAGS);
		
		if (StringUtils.isBlank(base) || StringUtils.isBlank(path)
				|| StringUtils.isBlank(type)) {
			res.setStatus(Status.STATUS_BAD_REQUEST);
			Map<String, Object> jsonResponse = new HashMap<String, Object>();
			jsonResponse.put("message", "garbage in garbage out");
			JSONValue.writeJSONString(jsonResponse, res.getWriter());
			return;
		}

		String parentDir = FilenameUtils.getFullPathNoEndSeparator(path);
		String name = FilenameUtils.getName(path);
		NodeRef parentRef = locateNode(base, parentDir);
		if (parentRef == null) {
			res.setStatus(Status.STATUS_NOT_FOUND);
			Map<String, Object> jsonResponse = new HashMap<String, Object>();
			jsonResponse.put("message", "parent directory does not exists");
			JSONValue.writeJSONString(jsonResponse, res.getWriter());
			return;
		}

		try {
			QName fqtn = QName.resolveToQName(
					serviceRegistry.getNamespaceService(), type);
			FileInfo finfo = serviceRegistry.getFileFolderService().create(
					parentRef, name, fqtn);
			if (!finfo.isFolder() && !finfo.isLink()) {
				// create 0 byte content to have mimetype mapping from name
				// there is also no no-content semantic on fs
				ContentWriter writer = serviceRegistry.getContentService()
						.getWriter(finfo.getNodeRef(),
								ContentModel.PROP_CONTENT, true);
				writer.guessMimetype(name);
				writer.setEncoding("UTF-8");
				writer.putContent("");
				
				//LockType lockType = null;
				//if(isSet(flags, O_WRONLY) || isSet(flags, O_RDWR)) {
				//	lockType = LockType.WRITE_LOCK;
				//} else {
				//	lockType = LockType.READ_ONLY_LOCK;
				//}
				//serviceRegistry.getLockService().lock(finfo.getNodeRef(), lockType);
			}

			Map<String, Object> jsonResponse = new HashMap<String, Object>();
			jsonResponse.put("nodeRef", finfo.getNodeRef().toString());
			jsonResponse.put("uuid", finfo.getNodeRef().getId());
			JSONValue.writeJSONString(jsonResponse, res.getWriter());
		} catch (FileExistsException ex) {
			sendError(res, "file or directory already exists", ERROR_EXIST, Status.STATUS_CONFLICT);
			return;
		}
	}

}
