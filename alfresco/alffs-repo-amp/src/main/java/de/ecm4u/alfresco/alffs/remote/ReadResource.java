package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.nio.channels.Channels;
import java.nio.channels.FileChannel;
import java.nio.channels.WritableByteChannel;

import org.alfresco.model.ContentModel;
import org.alfresco.service.cmr.repository.ContentReader;
import org.alfresco.service.cmr.repository.NodeRef;
import org.apache.commons.lang.StringUtils;
import org.json.JSONException;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public class ReadResource extends AbstractIOResource {
	@Override
	public void doGet(WebScriptRequest req, WebScriptResponse res)
			throws IOException, JSONException {
		res.setContentType(BIN_MIMETYPE);
		res.setStatus(Status.STATUS_OK);

		String base = req.getParameter(PARAM_BASE);
		String path = req.getParameter(PARAM_PATH);
		String sizeStr = req.getParameter(PARAM_SIZE);
		String offsetStr = req.getParameter(PARAM_OFFSET);
		String headerEtag = req.getHeader(HEADER_IF_NONE_MATCH);

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

		ContentReader reader = serviceRegistry.getContentService().getReader(
				nodeRef, ContentModel.PROP_CONTENT);
		if (reader == null || !reader.exists()) {
			res.setHeader("Content-Length", "0");
			return;
		}

		long size = reader.getSize();
		long offset = 0;
		long servedSize = 0;
		
		if (StringUtils.isBlank(sizeStr) && StringUtils.isBlank(offsetStr)) {
			String url = reader.getContentData().getContentUrl();
			String etag = encodeContentEtag(url);
			// add etag if whole file is transfered
			// because alfrescos content store is copy on write the content url
			// can be used as
			// an etag
			if (StringUtils.isNotBlank(headerEtag)) {
				headerEtag = headerEtag.replaceAll("\"", "");
				// matching etag -> 304 and no content transfer
				if (etag.equals(headerEtag)) {
					res.setStatus(Status.STATUS_NOT_MODIFIED);
					return;
				}
			}
			res.setHeader("Etag", "\"" + etag + "\"");
			servedSize = size;
		} else {
			offset = Long.parseLong(offsetStr);
			long requestedSize = Long.parseLong(sizeStr);
			if(offset + requestedSize > size) {
				servedSize = size - offset;
			} else {
				servedSize = requestedSize;
			}
		}

		res.setHeader("Content-Length", "" + servedSize);

		res.setContentType(reader.getMimetype());
		FileChannel fch = reader.getFileChannel();
		try {
			WritableByteChannel wch = Channels
					.newChannel(res.getOutputStream());
			fch.transferTo(offset, size, wch);
		} finally {
			fch.close();
		}

		return;
	}

	
}
