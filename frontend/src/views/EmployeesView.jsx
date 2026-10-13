import '../styles/Contents.css';
import '../styles/Employee.css';

export const EmployeesView = ({ data, loading, connected }) => {
  if (loading) {
    return <h1 style={{ fontSize: '5rem', color: '#ccc' }}>Loading data from Backend ...</h1>;
  }

  if (!data || data.length === 0) {
    return <div className="empty-state">No staff</div>;
  }

  let numberOrder = 0

  const serverStatus = connected
    ? "🟢"
    : "🔴";

  return (
    <div className="employee-view-container">
      <div className="employee-header">
        <h2>
          Employees management
        </h2>
        <h6>
          {serverStatus}
        </h6>
      </div>
      <table className="employee-table">
        <thead>
          <tr>
            <th className="employee-table-th">ID</th>
            <th className="employee-table-th">Fullname</th>
            <th className="employee-table-th">Email</th>
            <th className="employee-table-th">Department</th>
            <th className="employee-table-th">Role</th>
            <th className="employee-table-th">Permission</th>
            <th className="employee-table-th">Status</th>
          </tr>
        </thead>
        <tbody>
          {data.map((emp) => (
            <tr key={emp.id}>
              <td className="employee-table-td">{++numberOrder}</td>
              <td className="employee-table-td"><b>{emp.username}</b></td>
              <td className="employee-table-td"><u>{emp.email}</u></td>
              <td className="employee-table-td">{emp.dept}</td>
              <td className="employee-table-td">{emp.role.charAt(0).toUpperCase() + emp.role.slice(1)}</td>
              <td className="employee-table-td">{emp.permissions.charAt(0).toUpperCase() + emp.permissions.slice(1)}</td>

              <td className="employee-table-td">
                <span className={`employee-status-badge ${emp?.status?.toLowerCase() || "Null"}`}>
                  {emp.status.charAt(0).toUpperCase() + emp.status.slice(1)}
                </span>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
};

export default EmployeesView;