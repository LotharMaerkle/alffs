package de.ecm4u.alfresco.alffs.remote;

import java.io.IOException;
import java.util.Date;
import java.util.HashMap;
import java.util.Map;

import org.alfresco.model.ContentModel;
import org.alfresco.service.cmr.model.FileInfo;
import org.alfresco.service.cmr.repository.ContentData;
import org.alfresco.service.cmr.repository.NodeRef;
import org.apache.commons.lang.StringUtils;
import org.json.JSONException;
import org.json.simple.JSONValue;
import org.springframework.extensions.webscripts.Status;
import org.springframework.extensions.webscripts.WebScriptRequest;
import org.springframework.extensions.webscripts.WebScriptResponse;

public class StatResource extends AbstractIOResource {
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
		jsonResponse.put("st_dev", "dev");
		jsonResponse.put("st_ino", nodeRef.toString());

		/*
		 * S_IFMT 0170000 bit mask for the file type bit fields S_IFSOCK 0140000
		 * socket S_IFLNK 0120000 symbolic link S_IFREG 0100000 regular file
		 * S_IFBLK 0060000 block device S_IFDIR 0040000 directory S_IFCHR
		 * 0020000 character device S_IFIFO 0010000 FIFO S_ISUID 0004000 set UID
		 * bit S_ISGID 0002000 set-group-ID bit (see below) S_ISVTX 0001000
		 * sticky bit (see below) S_IRWXU 00700 mask for file owner permissions
		 * S_IRUSR 00400 owner has read permission S_IWUSR 00200 owner has write
		 * permission S_IXUSR 00100 owner has execute permission S_IRWXG 00070
		 * mask for group permissions S_IRGRP 00040 group has read permission
		 * S_IWGRP 00020 group has write permission S_IXGRP 00010 group has
		 * execute permission S_IRWXO 00007 mask for permissions for others (not
		 * in group) S_IROTH 00004 others have read permission S_IWOTH 00002
		 * others have write permission S_IXOTH 00001 others have execute
		 * permission
		 */

		FileInfo finfo = serviceRegistry.getFileFolderService().getFileInfo(
				nodeRef);
		int flags = 0;
		int mode = 0;
		if (finfo.isLink()) {
			flags = S_IFLNK;
			mode = 0644;
			jsonResponse.put("st_nlink", 1);
		} else if (finfo.isFolder()) {
			flags = S_IFDIR;
			mode = 0755;
			// at least 2, each subdir will add 1
			jsonResponse.put("st_nlink", 2);
		} else {
			flags = S_IFREG;
			mode = 0644;
			jsonResponse.put("st_nlink", serviceRegistry.getNodeService()
					.getParentAssocs(nodeRef).size());
		}

		jsonResponse.put("st_mode", flags | mode);
		jsonResponse.put("st_uid", serviceRegistry.getOwnableService()
				.getOwner(nodeRef));
		// no st_gid in alfresco
		long blocks = 0;
		if (flags == S_IFREG) {
			ContentData cdata = finfo.getContentData();
			long size = 0;

			if (cdata != null) {
				size = cdata.getSize();
			}
			jsonResponse.put("st_size", size);
			blocks = 1 + (size / 512L);
		}
		Date atime = (Date) serviceRegistry.getNodeService().getProperty(
				nodeRef, ContentModel.PROP_ACCESSED);
		if (atime != null) {
			jsonResponse.put("st_atime", formatIsoUTC(atime));
			jsonResponse.put("st_atime_epoch_sec", Long.toString(atime.getTime() / 1000L));
		}
		Date mtime = (Date) serviceRegistry.getNodeService().getProperty(
				nodeRef, ContentModel.PROP_MODIFIED);
		if (mtime != null) {
			jsonResponse.put("st_mtime", formatIsoUTC(mtime));
			jsonResponse.put("st_mtime_epoch_sec", Long.toString(mtime.getTime() / 1000L));
		}
		//linux ctime is not create time - it is the inode change time
		//there is maybe a birthtime or crtime in stat calls
		Date ctime = (Date) serviceRegistry.getNodeService().getProperty(
				nodeRef, ContentModel.PROP_CREATED);
		if (ctime != null) {
			jsonResponse.put("st_ctime", formatIsoUTC(ctime));
			jsonResponse.put("st_ctime_epoch_sec", Long.toString(ctime.getTime() / 1000L));
		}
		
		
		// preferred io block size
		jsonResponse.put("st_blksize", 4096);
		jsonResponse.put("st_blocks", blocks);
		JSONValue.writeJSONString(jsonResponse, res.getWriter());

	}

}
