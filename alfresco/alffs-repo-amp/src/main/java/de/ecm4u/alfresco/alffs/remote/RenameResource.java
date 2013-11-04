package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.util.Map;

import org.alfresco.service.cmr.model.FileExistsException;
import org.alfresco.service.cmr.model.FileNotFoundException;
import org.alfresco.service.cmr.repository.NodeRef;
import org.alfresco.service.namespace.QName;
import org.apache.commons.io.FilenameUtils;
import org.apache.commons.lang.StringUtils;
import org.json.JSONException;
import org.json.simple.JSONValue;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public class RenameResource extends AbstractIOResource {
	@SuppressWarnings("unchecked")
	@Override
	public void doPost(WebScriptRequest req, WebScriptResponse res)
			throws IOException, JSONException {
		res.setContentType(JSON_MIMETYPE);
		res.setContentEncoding(DEFAULT_ENCODING);
		
		
		res.setStatus(Status.STATUS_OK);

		Map<String, Object> jsonParam = (Map<String, Object>) JSONValue
				.parse(req.getContent().getContent());

		String base = req.getParameter(PARAM_BASE);
		String path = req.getParameter(PARAM_PATH);
		String newpath = (String) jsonParam.get(PARAM_NEWPATH);

		if (StringUtils.isBlank(base) || StringUtils.isBlank(path)
				|| StringUtils.isBlank(newpath)) {
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

		NodeRef targetRef = locateNode(base, newpath);
		QName targetType = null;
		if(targetRef != null) {
			targetType = serviceRegistry.getNodeService().getType(nodeRef);
		}
		String parentDirPath = FilenameUtils.getFullPathNoEndSeparator(newpath);
		String newName = FilenameUtils.getName(newpath);
		NodeRef targetParentRef = locateNode(base, parentDirPath);
		if (targetParentRef == null) {
			// target parent has to exist
			sendError(res, "no such file or directory", ERROR_NOENT);
			return;
		}
		String sourceParentDirPath = FilenameUtils.getFullPathNoEndSeparator(path);
		boolean sameDirRename = sourceParentDirPath.equals(parentDirPath);
		
		QName type = serviceRegistry.getNodeService().getType(nodeRef);
		if (isDirectory(type)) {
			// target does not exists or is empty dir
			if (targetRef != null) {
				if(!isDirectory(targetType)) {
					sendError(res, "not a directory", ERROR_NOTDIR);
					return;
				}
				int noChild = serviceRegistry.getNodeService()
						.countChildAssocs(targetRef, false);
				if (noChild == 0) {
					// atomic rename with alfresco :)
					serviceRegistry.getNodeService().deleteNode(targetRef);
				} else {
					sendError(res, "directory not empty", ERROR_NOTEMPTY);
					return;
				}
			}

			// there is a race here as the directory could have been created in
			// the meantime
			try {
				serviceRegistry.getFileFolderService().move(nodeRef,
						sameDirRename ? null : 
								targetParentRef, newName);
			} catch (FileExistsException e) {
				sendError(res, "directory exists", ERROR_EXIST);
				return;
			} catch (FileNotFoundException e) {
				sendError(res, "no such file or directory", ERROR_NOENT);
				return;	
			}
		} else if (isFile(type)) {
			if (targetRef != null) {
				if(!isFile(targetType)) {
					sendError(res, "is a directory", ERROR_ISDIR);
					return;
				}

				serviceRegistry.getNodeService().deleteNode(targetRef);
			}
			// there is a race here as the directory could have been created in
			// the meantime
			try {
				serviceRegistry.getFileFolderService().move(nodeRef,
						sameDirRename ? null : targetParentRef, newName);
			} catch (FileExistsException e) {
				sendError(res, "file exists", ERROR_EXIST);
				return;
			} catch (FileNotFoundException e) {
				sendError(res, "nu such file or directory", ERROR_NOENT);
				return;
			}
			
		} else {
			throw new RuntimeException("unkown type");
		}

	}
}
