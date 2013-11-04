package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.nio.file.FileStore;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.HashMap;
import java.util.Map;

import org.alfresco.repo.content.ContentStore;
import org.json.JSONException;
import org.json.simple.JSONValue;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public class StatFSResource extends AbstractIOResource {
	private ContentStore store;

	@Override
	public void doGet(WebScriptRequest req, WebScriptResponse res)
			throws IOException, JSONException {
		res.setContentType(JSON_MIMETYPE);
		res.setContentEncoding(DEFAULT_ENCODING);
		res.setStatus(Status.STATUS_OK);

		// the stores size methods where just wrong on osx
		
		String rootDir = store.getRootLocation();
		Path path = Paths.get(rootDir);
		FileStore fs = Files.getFileStore(path);

		Map<String, Object> jsonResponse = new HashMap<String, Object>();
		jsonResponse.put("freeBytes", Long.toString(fs.getUnallocatedSpace()));
		jsonResponse.put("totalBytes", Long.toString(fs.getTotalSpace()));
		jsonResponse.put("usableBytes", Long.toString(fs.getUsableSpace()));
		
		jsonResponse.put("maxFilename", 250); //windows limit
		jsonResponse.put("readOnly", !serviceRegistry.getTransactionService().getAllowWrite());
		JSONValue.writeJSONString(jsonResponse, res.getWriter());

	}

	public void setStore(ContentStore store) {
		this.store = store;
	}

}
