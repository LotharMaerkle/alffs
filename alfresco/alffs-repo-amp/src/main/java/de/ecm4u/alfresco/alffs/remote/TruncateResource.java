package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.channels.FileChannel;
import java.util.Map;

import org.alfresco.model.ContentModel;
import org.alfresco.service.cmr.repository.ContentWriter;
import org.alfresco.service.cmr.repository.NodeRef;
import org.apache.commons.lang.StringUtils;
import org.json.JSONException;
import org.json.simple.JSONValue;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public class TruncateResource extends AbstractIOResource {
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
		Long offset = (Long) jsonParam.get(PARAM_OFFSET);

		if (StringUtils.isBlank(base) || StringUtils.isBlank(path)
				|| offset == null) {
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

		ContentWriter writer = serviceRegistry.getContentService().getWriter(
				nodeRef, ContentModel.PROP_CONTENT, true);
		long size = writer.getSize();
		if (offset == 0) {
			FileChannel fch = writer.getFileChannel(true);
			fch.close();
			// maybe set empty content is better
		} else {
			FileChannel fch = writer.getFileChannel(false);
			try {
				if (offset < size) {
					fch.truncate(offset);
				} else if (offset > size) {
					// fill with nulls see man 2 truncate
					// use fancy loop to cope with large offsets
					fch.position(size);
					byte[] buffer = new byte[4096];
					long totalToWrite = offset - size;
					long written = 0;
					while (written < totalToWrite) {
						int length = 0;
						long missing = totalToWrite - written;
						if (missing >= buffer.length) {
							length = buffer.length;
						} else {
							length = (int) missing;
						}
						ByteBuffer src = ByteBuffer.wrap(buffer, 0, length);
						written += fch.write(src);
					}

					fch.close();

					// invariant
					if (offset != (size + written)) {
						throw new RuntimeException();
					}

				}
			} finally {
				fch.close();
			}
		}
	}
}
